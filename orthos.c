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
    struct avl_tree* dirs;

    if( argc < 2 ){
        printf( "Usage: %s file [file2 .. fileN]\n", ARGV[0] );
        return 1;
    }

    if( -1 == (handle = inotify_init()) ){
        e = errno;
        perror( "Failed to initialize inotify");
        return e;
    }

    /* new tree which will use event_cmp to compare */
    if( NULL == (dirs = avl_create(event_cmp)) ){
        e = errno;
        perror( "Failed to initialize inotify");
        return e;
    }
    
    struct event_map* a = malloc(sizeof(struct event_map*));
    a->wd = 1;
    struct event_map* b = malloc(sizeof(struct event_map*));
    b->wd = 2;
    avl_insert(dirs, a);
    avl_insert(dirs, b);
    return 0;

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

signed int event_cmp(
        const struct event_map* const a,
        const struct event_map* const b
        ){
    if( a == NULL && b == NULL ){
        return 0;
    }
    if( a == NULL ){
        return -1;
    }
    if( b == NULL ){
        return 1;
    }
    if( a->wd > b->wd ){
        return -1;
    }
    if( a->wd < b->wd ){
        return 1;
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

/* 
 * rebalancing includes updating the balance fator of a node.  The balance
 * factor is the height of the right tree minus the height of the left tree.
 * So, the factor is negative if the left is taller, positive if the right is
 * taller, and 0 if they're equal.
 */

/*
 * create a new avl tree
 */
struct avl_tree* avl_create( signed int (*cmp)(const void*, const void*) ) {
    struct avl_tree* tree;

    if( NULL == (tree = malloc( sizeof(*tree) )) ){
        return NULL;
    }
     /* init tree */
    tree->root = NULL;
    tree->compare = cmp;

    return tree;
}

/*
 * return the data field from a tree node
 */
void * avl_lookup(
        const struct avl_tree* const tree, 
        const void* const data
        ){
    struct avl_tree_node* n;
    int cmp;

    //assert( tree != NULL && data != NULL );
    if( tree == NULL || data == NULL ){
        errno = EINVAL;
        return NULL;
    }

    for( n = tree->root; n != NULL; ){
        cmp = tree->compare( data, n->data);
        if( cmp > 0 ){
            n = n->right;
        }
        else if( cmp < 0 ){
            n = n->left;
        }
        else{
            return n->data;
        }
    }

    /* not found */
    return NULL;
}

void* avl_insert(
        struct avl_tree* tree,
        void* data // this may actually end up changed; we store a pointer
        ){
    int cmp;
    struct avl_tree_node* n;  /* current node */
    struct avl_tree_node* np; /* node's parent */
    struct avl_tree_node* z;  /* last non-zero node */
    struct avl_tree_node* zp; /* last non-zero node's parent */
    struct avl_tree_node* newnode;

    /* track path we took to the insertion so we can adjust balance */
    /* using a linked list because arrays are limiting */
    /* I might use all of your memory! All of it! Bwa hah ha ha ha! */
    struct path_element {
        struct path_element  *previous;
        struct avl_tree_node *parent_node;
        struct avl_tree_node *node;
        int direction;
    };
    struct path_element* path;
    struct path_element* p;

    /* sanity check */
    if( tree == NULL || data == NULL ){
        /* explode! */
        return NULL;
    }

    n  = tree->root;
    np = n;
    //z  = n; /* should z / zp start at NULL instead? */
    //zp = NULL;
    /* find insertion point */
    while( n != NULL ){
        cmp = tree->compare( data, n->data );
        // if they match, then we're done
        if( cmp == 0 ){
            return n->data;
        }

        /*
        if( n->balance != 0 ){
            z  = n;
            zp = np;
        }
        */

        // Add a new component to the path
        p = malloc(sizeof(*p));
        p->previous    = path;
        p->direction   = cmp;
        p->parent_node = np;
        p->node        = n;
        /* I got to "node" from "parent_node" in direction "cmp" */
        path = p;

        // On to the next node!
        np = n;
        n = (cmp < 0 ) ? n->left : n->right;
    }

    /* create and insert node */
    if( NULL == (newnode = malloc(sizeof( *newnode ))) ){
        // maybe set errno?  It's probably already set.
        return NULL;
    }
    newnode->data  = data;
    newnode->left  = NULL;
    newnode->right = NULL;
    newnode->balance = 0;

    if( np == NULL ){
        /* we never entered the loop and the tree is empty */
        tree->root = newnode;
    }
    else if( cmp < 0 ){
        np->left = newnode;
    }
    else{
        np->right = newnode;
    }

    printf("data: '%i'\n", (int)data);
    printf("new pointer: '%i'\n", (int)(newnode->data));
    printf("root pointer: '%i'\n", (int)(tree->root->data));
    if( path == NULL ){
        /* we never entered the loop, but the tree is not empty */
        return newnode->data;
    }

        /* update balance factor */
    /* rebalance */
    printf("Ok!\n");
}

/* end orthos.c */
