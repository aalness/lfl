/*
 * $Id: term.cpp 1336 2014-12-08 09:29:59Z justin $
 * Copyright (C) 2009 Lucid Fusion Labs

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "lfapp/lfapp.h"
#include "lfapp/dom.h"
#include "lfapp/css.h"
#include "lfapp/flow.h"
#include "lfapp/gui.h"
#include "lfapp/ipc.h"
#include "crawler/html.h"
#include "crawler/document.h"

#include <sys/socket.h>

namespace LFL {
DEFINE_int   (peak_fps,    50,    "Peak FPS");
DEFINE_bool  (draw_fps,   false,  "Draw FPS");
DEFINE_string(command,    "",     "Execute initial command");
DEFINE_string(screenshot, "",     "Screenshot and exit");

extern FlagOfType<string> FLAGS_default_font_;
extern FlagOfType<bool>   FLAGS_lfapp_network_;

Scene scene;
BindMap *binds;
unordered_map<string, Shader> shader_map;
Browser *image_browser;
NetworkThread *network_thread;
ProcessAPIServer *render_process;
int new_win_width = 80*10, new_win_height = 25*17;

void MyNewLinkCB(const shared_ptr<TextGUI::Link> &link) {
    const char *args = FindChar(link->link.c_str() + 6, isint2<'?', ':'>);
    string image_url(link->link, 0, args ? args - link->link.c_str() : string::npos);
    // if (SuffixMatch(image_url, ".gifv")) return;
    if (!FileSuffix::Image(image_url)) {
        return;
        string prot, host, port, path;
        if (HTTP::ParseURL(image_url.c_str(), &prot, &host, &port, &path) &&
            SuffixMatch(host, "imgur.com") && !FileSuffix::Image(path)) {
            image_url += ".jpg";
        } else return;
    }
    image_url += BlankNull(args);
    if (network_thread) network_thread->Write(new Callback([=]() { link->image = image_browser->doc.parser->OpenImage(image_url); }));
}

void MyHoverLinkCB(TextGUI::Link *link) {
    Texture *tex = link ? link->image.get() : 0;
    if (!tex) return;
    tex->Bind();
    screen->gd->SetColor(Color::white - Color::Alpha(0.2));
    Box::DelBorder(screen->Box(), screen->width*.2, screen->height*.2).Draw(tex->coord);
}

struct ReadBuffer {
    string data; int size; Time stamp;
    ReadBuffer(int S=0) : size(S), data(S, 0), stamp(Now()) {}
    void Reset() { data.resize(size); stamp=Now(); }
};

struct MyTerminalWindow {
    ProcessPipe process;
    ReadBuffer read_buf;
    Terminal *terminal=0;
    Shader *activeshader;
    int font_size;
    bool effects_mode=0, read_pending=0, join_read_pending=0, effects_init=0;
    FrameBuffer *effects_buffer=0;

    MyTerminalWindow() : read_buf(65536), activeshader(&app->video.shader_default), font_size(FLAGS_default_font_size) {}
    ~MyTerminalWindow() { if (process.in) app->scheduler.DelWaitForeverSocket(fileno(process.in)); }

    void Open() {
        int fd = -1;
#ifndef FUZZ_DEBUG
        setenv("TERM", "screen", 1);
        string shell = BlankNull(getenv("SHELL"));
        CHECK(!shell.empty());
        const char *av[] = { shell.c_str(), 0 };
        CHECK_EQ(process.OpenPTY(av), 0);
        fd = fileno(process.out);
        app->scheduler.AddWaitForeverMouse();
        app->scheduler.AddWaitForeverSocket(fd, SocketSet::READABLE, 0);
        if (int len = FLAGS_command.size()) CHECK_EQ(len+1, write(fd, StrCat(FLAGS_command, "\n").data(), len+1));
#endif

        terminal = new Terminal(fd, screen, Fonts::Get(FLAGS_default_font, "", font_size));
        terminal->new_link_cb = MyNewLinkCB;
        terminal->hover_link_cb = MyHoverLinkCB;
        terminal->active = true;
        terminal->SetDimension(80, 25);

#ifdef FUZZ_DEBUG
        for (int i=0; i<256; i++) {
            INFO("fuzz i = ", i);
            for (int j=0; j<256; j++)
                for (int k=0; k<256; k++)
                    terminal->Write(string(1, i), 1, 1);
        }
        terminal->Newline(1);
        terminal->Write("Hello world.", 1, 1);
#endif
    }
    void UpdateTargetFPS() {
        effects_mode = CustomShader() || screen->console->animating;
        int target_fps = effects_mode ? FLAGS_peak_fps : 0;
        if (target_fps != screen->target_fps) app->scheduler.UpdateTargetFPS(target_fps);
    }
    bool CustomShader() const { return activeshader != &app->video.shader_default; }
};

int Frame(Window *W, unsigned clicks, unsigned mic_samples, bool cam_sample, int flag) {
    static const int join_read_size = 1024;
    static const Time join_read_interval(100), refresh_interval(33);
    MyTerminalWindow *tw = (MyTerminalWindow*)W->user1;
    tw->read_buf.Reset();
    if (tw->process.in && NBRead(fileno(tw->process.in), &tw->read_buf.data)) {
        tw->terminal->Write(tw->read_buf.data);
        tw->read_pending = 1;
    }
    if (tw->read_buf.data.size() && !(flag & LFApp::Frame::DontSkip)) {
        bool join_read = tw->read_buf.data.size() == join_read_size;
        if (join_read) { tw->join_read_pending=1; if (app->scheduler.WakeupIn(0, join_read_interval)) return -1; }
        else        if (!tw->join_read_pending) { if (app->scheduler.WakeupIn(0,   refresh_interval)) return -1; }
    }
    app->scheduler.ClearWakeupIn();
    W->gd->DrawMode(DrawMode::_2D);
    W->gd->DisableBlend();
    bool effects = tw->CustomShader(), draw = true;
    if (effects) {
        if (!tw->effects_buffer) {
            tw->effects_buffer = new FrameBuffer();
            tw->effects_buffer->Create(512, 512);
            tw->effects_buffer->tex.Create(tw->effects_buffer->width, tw->effects_buffer->height);
            tw->effects_buffer->Attach(tw->effects_buffer->tex.ID);
        } else if ((draw = tw->read_pending)) tw->effects_buffer->Attach();
    }
    if (draw) tw->terminal->Draw(effects ? tw->effects_buffer->tex.Dimension() : W->Box(), true);
    if (effects) {
        if (draw) tw->effects_buffer->Release();
        tw->effects_buffer->tex.Bind();
        glTimeResolutionShader(tw->activeshader, &tw->effects_buffer->tex);
        screen->Box().Draw(tw->effects_buffer->tex.coord);
        screen->gd->UseShader(0);
    }
    W->DrawDialogs();
    if (FLAGS_draw_fps) Fonts::Default()->Draw(StringPrintf("FPS = %.2f", FPS()), point(W->width*.85, 0));
    if (FLAGS_screenshot.size()) ONCE(app->shell.screenshot(vector<string>(1, FLAGS_screenshot)); app->run=0;);
    tw->read_pending = tw->join_read_pending = 0;
    return 0;
}

void SetFontSize(int n) {
    MyTerminalWindow *tw = (MyTerminalWindow*)screen->user1;
    tw->font_size = n;
    tw->terminal->font = Fonts::Get(FLAGS_default_font, "", tw->font_size);
    screen->Reshape(tw->terminal->font->FixedWidth() * tw->terminal->term_width,
                    tw->terminal->font->Height()     * tw->terminal->term_height);
}
void MyConsoleAnimating(Window *W) { 
    ((MyTerminalWindow*)W->user1)->UpdateTargetFPS();
    if (!screen->console->animating) {
        if (screen->console->active) app->scheduler.AddWaitForeverKeyboard();
        else                         app->scheduler.DelWaitForeverKeyboard();
    }
}
void MyIncreaseFontCmd(const vector<string>&) { SetFontSize(((MyTerminalWindow*)screen->user1)->font_size + 1); }
void MyDecreaseFontCmd(const vector<string>&) { SetFontSize(((MyTerminalWindow*)screen->user1)->font_size - 1); }
void MyColorsCmd(const vector<string> &arg) {
    string colors_name = arg.size() ? arg[0] : "";
    MyTerminalWindow *tw = (MyTerminalWindow*)screen->user1;
    if      (colors_name ==       "vga") tw->terminal->SetColors(Singleton<Terminal::StandardVGAColors>::Get());
    else if (colors_name == "solarized") tw->terminal->SetColors(Singleton<Terminal::SolarizedColors>  ::Get());
    tw->terminal->Redraw();
}
void MyShaderCmd(const vector<string> &arg) {
    string shader_name = arg.size() ? arg[0] : "";
    MyTerminalWindow *tw = (MyTerminalWindow*)screen->user1;
    if (tw->effects_buffer) {
        tw->effects_buffer->tex.ClearGL();
        Replace(&tw->effects_buffer, (FrameBuffer*)0);
    }
    auto shader = shader_map.find(shader_name);
    tw->activeshader = shader != shader_map.end() ? &shader->second : &app->video.shader_default;
    tw->UpdateTargetFPS();
}

void MyInitFonts() {
    Video::InitFonts();
    string console_font = "VeraMoBd.ttf";
    Singleton<AtlasFontEngine>::Get()->Init(FontDesc(console_font, "", 32));
    FLAGS_console_font = StrCat("atlas://", console_font);
}

void MyWindowInitCB(Window *W) {
    W->width = new_win_width;
    W->height = new_win_height;
    W->caption = "Terminal";
    W->frame_cb = Frame;
    W->binds = binds;
}
void MyWindowStartCB(Window *W) {
    ((MyTerminalWindow*)W->user1)->Open();
    W->console->animating_cb = bind(&MyConsoleAnimating, screen);
}
void MyWindowCloneCB(Window *W) {
    W->InitConsole();
    W->user1 = new MyTerminalWindow();
    W->input_bind.push_back(W->binds);
    MyWindowStartCB(W);
}
void MyWindowClosedCB(Window *W) {
    delete (MyTerminalWindow*)W->user1;
    delete W;
}

}; // naemspace LFL
using namespace LFL;

extern "C" int main(int argc, const char *argv[]) {

    app->logfilename = StrCat(LFAppDownloadDir(), "term.txt");
    binds = new BindMap();
    MyWindowInitCB(screen);
    FLAGS_target_fps = 0;
    FLAGS_lfapp_video = FLAGS_lfapp_input = 1;
#ifdef __APPLE__
    FLAGS_font_engine = "coretext";
#else
    FLAGS_font_engine = "freetype";
#endif

    if (app->Create(argc, argv, __FILE__)) { app->Free(); return -1; }
    if (!FLAGS_lfapp_network_.override) FLAGS_lfapp_network = 1;
    if (FLAGS_lfapp_network) {
        string render_client = StrCat(app->BinDir(), "lterm-sandbox-render");
        render_process = new ProcessAPIServer();
        render_process->Start(render_client);
    }

    if (FLAGS_font_engine != "atlas") app->video.init_fonts_cb = &MyInitFonts;
    if (FLAGS_default_font_.override) {
    } else if (FLAGS_font_engine == "coretext") {
        FLAGS_default_font = "Monaco";
        FLAGS_default_font_size = 15;
    } else if (FLAGS_font_engine == "freetype") { 
        FLAGS_default_font = "VeraMoBd.ttf"; // "DejaVuSansMono-Bold.ttf";
        FLAGS_default_missing_glyph = 42;
    } else if (FLAGS_font_engine == "atlas") {
        FLAGS_default_font = "VeraMoBd.ttf";
        FLAGS_default_missing_glyph = 42;
        // FLAGS_default_font_size = 32;
    }
    FLAGS_atlas_font_sizes = "32";

    if (app->Init()) { app->Free(); return -1; }
    app->window_init_cb = MyWindowInitCB;
    app->window_closed_cb = MyWindowClosedCB;
    app->shell.command.push_back(Shell::Command("colors", bind(&MyColorsCmd, _1)));
    app->shell.command.push_back(Shell::Command("shader", bind(&MyShaderCmd, _1)));
    if (FLAGS_lfapp_network) {
        CHECK((network_thread = app->CreateNetworkThread()));
        network_thread->Write(new Callback([&](){ Video::CreateGLContext(screen); }));
    }

    app->create_win_f = bind(&Application::CreateNewWindow, app, &MyWindowCloneCB);
    binds->Add(Bind('n', Key::Modifier::Cmd, Bind::CB(app->create_win_f)));
    binds->Add(Bind('=', Key::Modifier::Cmd, Bind::CB(bind(&MyIncreaseFontCmd, vector<string>()))));
    binds->Add(Bind('-', Key::Modifier::Cmd, Bind::CB(bind(&MyDecreaseFontCmd, vector<string>()))));
    binds->Add(Bind('6', Key::Modifier::Cmd, Bind::CB(bind([&](){ Window::Get()->console->Toggle(); }))));

    vector<pair<string,string>> effects_menu = { {"None", "shader none"}, {"Warper", "shader warper"},
        { "Water", "shader water" }, { "Twistery", "shader twistery" }, { "Fire", "shader fire" },
        { "Waves", "shader waves" }, { "Emboss", "shader emboss" }, { "Stormy", "shader stormy" },
        { "Alien", "shader alien" }, { "Fractal", "shader fractal" }, { "Shrooms", "shader shrooms" } };
    app->AddNativeMenu("Effects", effects_menu);

    Shader::Create("warper", screen->gd->vertex_shader, Asset::FileContents("warper.glsl"), ShaderDefines(1,0,1,0), &shader_map["warper"]);
    Shader::CreateShaderToy("water", Asset::FileContents("water.glsl"), &shader_map["water"]);
    Shader::CreateShaderToy("twistery", Asset::FileContents("twistery.glsl"), &shader_map["twistery"]);
    Shader::CreateShaderToy("fire", Asset::FileContents("fire.glsl"), &shader_map["fire"]);
    Shader::CreateShaderToy("waves", Asset::FileContents("waves.glsl"), &shader_map["waves"]);
    Shader::CreateShaderToy("emboss", Asset::FileContents("emboss.glsl"), &shader_map["emboss"]);
    Shader::CreateShaderToy("stormy", Asset::FileContents("stormy.glsl"), &shader_map["stormy"]);
    Shader::CreateShaderToy("alien", Asset::FileContents("alien.glsl"), &shader_map["alien"]);
    Shader::CreateShaderToy("fractal", Asset::FileContents("fractal.glsl"), &shader_map["fractal"]);
    Shader::CreateShaderToy("shrooms", Asset::FileContents("shrooms.glsl"), &shader_map["shrooms"]);

    image_browser = new Browser();
    image_browser->doc.parser->render_process = render_process;
    MyTerminalWindow *tw = new MyTerminalWindow();
    screen->user1 = tw;
    MyWindowStartCB(screen);
    SetFontSize(tw->font_size);
    new_win_width  = tw->terminal->font->FixedWidth() * tw->terminal->term_width,
    new_win_height = tw->terminal->font->Height()     * tw->terminal->term_height;
    tw->terminal->Draw(screen->Box(), false);
    INFO("Starting Terminal ", FLAGS_default_font, " (w=", tw->terminal->font->fixed_width,
                                                   ", h=", tw->terminal->font->Height(), ")");

    app->scheduler.Start();
    return app->Main();
}
