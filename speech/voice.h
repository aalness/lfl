/*
 * $Id: voice.h 1306 2014-09-04 07:13:16Z justin $
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

#ifndef __LFL_LFAPP_VOICE_H__
#define __LFL_LFAPP_VOICE_H__
namespace LFL {

/* unit database */
struct VoiceModel {
    struct Unit {
        WavReader wav;
        int samples;
        struct Sample { int offset, len; } *sample;
        Unit() : samples(0), sample(0) {}
        ~Unit() { delete wav.f; free(sample); }
    } unit[LFL_PHONES];

    int read(const char *dir);
    RingBuf *synth(const char *text, int start=0);

    int nextPhone(int phone, int lastphone, int lastphoneindex);
};

}; // namespace LFL
#endif // __LFL_LFAPP_VOICE_H__
