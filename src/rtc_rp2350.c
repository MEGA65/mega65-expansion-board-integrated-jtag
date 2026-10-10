/* rtc.c
Copyright 2021 Carl John Kugler III

Licensed under the Apache License, Version 2.0 (the License); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at

   http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an AS IS BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
*/

#include "pico.h"
#include <time.h>
#include "pico/aon_timer.h"
#include "ff.h"
#include "rtc.h"

void time_init(void) {
    if (!aon_timer_is_running()) {
        struct timespec ts = { .tv_sec = 1735689600, .tv_nsec = 0 }; /*
        2025-01-01 */
        aon_timer_start(&ts);
    }
}

DWORD get_fattime(void) {
    struct timespec ts;
    if (!aon_timer_get_time(&ts)) return 0;
    struct tm t;
    gmtime_r(&ts.tv_sec, &t);
    return ((DWORD)(t.tm_year - 80) << 25) |
    ((DWORD)(t.tm_mon + 1)   << 21) |
    ((DWORD)t.tm_mday        << 16) |
    ((DWORD)t.tm_hour        << 11) |
    ((DWORD)t.tm_min         <<  5) |
    ((DWORD)t.tm_sec         >>  1);
}
