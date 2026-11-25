/*******************************************************************************
 * $Id$
 *******************************************************************************
 * Orthos
 * Watch a Subversion export and log differences from SVN HEAD.
 *
 *******************************************************************************
 * Copyright 2014 Danny Sauer
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef ORTHOS_H
#define ORTHOS_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

struct mismatch {
    struct mismatch* next;
    char* path;
    const char* reason;
    bool seen;
};

struct watch {
    struct watch* next;
    int wd;
    bool seen;
};

#endif /* orthos.h */
