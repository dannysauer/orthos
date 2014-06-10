/*******************************************************************************
 * $Id$
 *******************************************************************************
 * Orthos
 * A daemon to watch several files, alerting when they are modified without
 * prior authorization.
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

#include "orthos.h"

int main(int argc, char* ARGV[], char* ENV[]){
    int i=0, e=0;
    int len=0;
    int handle;
    char* fail_to_watch = "Failed to watch";
    char msg[NAME_MAX + sizeof(fail_to_watch) + 3]; /* space & 2x quotes */
    char buffer[EVENT_BUF_LEN];
    struct stat s;
    struct inotify_event* event;

    if( argc < 2 ){
        printf( "Usage: %s file [file2 .. fileN]\n", ARGV[0] );
        return 1;
    }

    if( -1 == (handle = inotify_init()) ){
        e = errno;
        perror( "Failed to initialize inotify");
        return e;
    }

    /*
     * add watches
     */
    for( i=1; i<argc; i++ ){
        // Verify that this is something we can watch
        if( -1 == lstat( ARGV[i], &s ) ){
            e = errno;
            snprintf( msg, sizeof(msg), "%s '%s'", fail_to_watch, ARGV[i] );
            perror( msg );
            // return e;
            continue;
        }
        if( ! S_ISDIR(s.st_mode) ){
            errno = ENOTDIR;
            e = errno;
            snprintf( msg, sizeof(msg), "%s '%s'", fail_to_watch, ARGV[i] );
            perror( msg );
            // return 1;
            continue;
        }

        /* DEBUG */
        printf("Monitoring '%s'\n", ARGV[i]);

        if( -1 == inotify_add_watch( handle, ARGV[i], IN_ALL_EVENTS ) ){
            e = errno;
            snprintf( msg, sizeof(msg), "%s '%s'", fail_to_watch, ARGV[i] );
            perror( msg );
            // return e;
        }
    }

    /*
     * actual monitoring
     */
    if( -1 == (len = read( handle, buffer, EVENT_BUF_LEN )) ){
        e = errno;
        perror( "Failed to read from inotify handle" );
        return e;
    }

    i = 0;
    while( i < len ){
        event = ( struct inotify_event* ) &buffer[ i ];
        printf( "Event detected on %s\n", event->name );
        i += EVENT_SIZE + event->len;
    }


    return 0;
}

/*
 * We need to store the "file descriptor to directory name" mappings in some
 * kind of structure.  In general, we'll expect to be looking up mappings more
 * often than we'll be creating new ones, so we can afford a minor trade-offs
 * in insertion speed compared to retreival speed.  We want to be somewhat
 * memory-efficient, though, so just allocating an array to hold all possible
 * integers is not a great idea. :)  I think that a balanced binary search tree
 * (an "AVL tree") makes for a good solution here.  So, here's an
 * implementation.  There's some good documentation on how AVL trees work
 * located at http://adtinfo.org/libavl.html/AVL-Trees.html
 *
 * The data stored is an event_map struct, which contains an integer watch
 * descriptor.  So, we'll use that as the sorted key.
 */


