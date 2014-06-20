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

#ifndef ORTHOS_H
#define ORTHOS_H

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/inotify.h>

#define EVENT_SIZE    ( sizeof( struct inotify_event ) )
/* 1024 events buffered */
#define EVENT_BUF_LEN ( 1024 * ( EVENT_SIZE + NAME_MAX + 1 ) )

/* descriptor->path mapping */
struct event_map {
    int wd;      /* watch descriptor */
    char path[]; /* directory associated with descriptor */
};

signed int event_cmp(const struct event_map*, const struct event_map*);

/* Danny's generic AVL tree stuff */
struct avl_tree {
    struct avl_tree_node* root;
    signed int (*compare)(const void*, const void*);
};

struct avl_tree_node {
    // struct avl_tree_node* parent;
    struct avl_tree_node* left;
    struct avl_tree_node* right;
    signed int balance;
    //struct event_map* data;
    void* data;
};

struct avl_tree* avl_create();
void* avl_insert(
        struct avl_tree*,
        void*
);

#endif /* orthos.h */
