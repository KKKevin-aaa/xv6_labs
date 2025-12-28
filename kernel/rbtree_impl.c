#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "rbtree.h"
#include "kvm.h"
/* basic rules for rbtree
* 1. node color can only be red or black
* 2. root should always be black, the same for leaf node(NULL)
* 3. red node's child node should be black(Red-red)
* 4. start by arbitry node to all leaf_node, these path should contain the same black node.(black-Height)
*/
static void __rb_rotate_left(rb_node_t *node, rb_root_t *root){
    rb_node_t *new_parent=node->rb_right, *left_node=new_parent->rb_left, *gparent=rb_parent(node);
    if(gparent==NULL)   root->rb_parent=new_parent;
    else if(gparent->rb_left==node){
        rb_set_parent(new_parent, gparent);
        gparent->rb_left=new_parent;
    }
    else{
        rb_set_parent(new_parent, gparent);
        gparent->rb_right=new_parent;
    }
    new_parent->rb_left=node;
    rb_set_parent(node, new_parent);
    node->rb_right=left_node;
    if(left_node!=NULL)    rb_set_parent(left_node, node);
}
static void __rb_rotate_right(rb_node_t *node, rb_root_t *root){
    rb_node_t *new_parent=node->rb_left;
    rb_node_t *right_node=new_parent->rb_right;
    rb_node_t *gparent=rb_parent(node);
    if(gparent==NULL)   root->rb_parent=new_parent;
    else if(gparent->rb_left==node){
        rb_set_parent(new_parent, gparent);
        gparent->rb_left=new_parent;
    }
    else{
        rb_set_parent(new_parent, gparent);
        gparent->rb_right=new_parent;
    }
    new_parent->rb_right=node;
    rb_set_parent(node, new_parent);
    node->rb_left=right_node;
    if(right_node!=NULL)    rb_set_parent(right_node, node);
}
//Reblance the tree after an insertion
void rb_insert_color(rb_node_t *node, rb_root_t *root){
    //Assume the new_added node is red
    rb_node_t *parent, *gparent;
    while((parent=rb_parent(node)) && rb_color(parent)==RB_RED){
        //1. parent should exist, or we reach root, exit 
        //2. parent should be black, or meet the need of "red-red"
        gparent=rb_parent(node);
        if(parent==gparent->rb_left){
            rb_node_t *uncle=gparent->rb_right;
            if(uncle && rb_color(uncle)==RB_RED){//uncle is red, change the color 
                rb_set_color(node, RB_BLACK);
                rb_set_color(uncle, RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node=gparent;
                continue;
            }
            //Uncle is black or NULL
            if(node==parent->rb_right){
                __rb_rotate_left(parent, root);
                rb_node_t *tmp=parent;
                parent=node;
                node=tmp;
            }
            rb_set_color(parent, RB_BLACK);
            rb_set_color(gparent, RB_RED);
            __rb_rotate_right(gparent, root);
        }
        else{
            rb_node_t *uncle=gparent->rb_left;
            if(uncle && rb_color(uncle)==RB_RED){
                rb_set_color(node, RB_BLACK);
                rb_set_color(uncle, RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node=gparent;
                continue;
            }
            //Uncle is black or NULL
            if(node==parent->rb_left){  //right-left
                __rb_rotate_right(parent, root);
                rb_node_t *tmp=parent;
                parent=node;
                node=tmp;
            }
            rb_set_color(parent, RB_BLACK);
            rb_set_color(gparent, RB_RED);
            __rb_rotate_left(gparent, root);
        }
    }
    rb_set_color(root->rb_parent, RB_BLACK);
}
/*
* Internal fixup function called by rb_erase
* This function rebalance the tree after a black node was removed, which would violate black-height
* params:@node is the replacement node that fill the gap, it's treated as double-black
* 'parent' is 'node's parents(must be passed by since 'node' can be NULL)
*/
static void __rb_erase_color(rb_node_t *node, rb_node_t *parent, rb_root_t *root){
    rb_node_t *other;  //slicing node
    //As long as 'node' is not the root and 'node' is black the loops continue
    while(node !=root->rb_parent && (node==NULL || rb_color(node)==RB_BLACK)){
        if(parent==NULL)    break;
        if(node==parent->rb_left){
            other=parent->rb_right;
            if(rb_color(other)==RB_RED){//sibling 'other' is red,transform first!
                rb_set_color(other, RB_BLACK);
                rb_set_color(parent,RB_RED);
                __rb_rotate_left(parent, root);
                other=parent->rb_right;
            }
            if((!other->rb_left || rb_color(other->rb_left)==RB_BLACK) &&
                (!other->rb_right || rb_color(other->rb_right)==RB_BLACK)){
                rb_set_color(other, RB_RED);
                node=parent;    //move the double-black up to the parent
                parent=rb_parent(node); //Continue loop from the parent
            }
            else{
                //other's right is black, other's left is red
                if(!other->rb_right || rb_color(other->rb_right)==RB_BLACK){
                    rb_set_color(other->rb_left, RB_BLACK);
                    rb_set_color(other, RB_RED);
                    __rb_rotate_right(other, root);
                    other=parent->rb_right;
                }
                //other's right is red
                rb_set_color(other, rb_color(parent));
                rb_set_color(parent, RB_BLACK);
                rb_set_color(other->rb_right, RB_BLACK);
                __rb_rotate_left(parent, root);
                node=root->rb_parent;break;
            }
        }
        else{
            other=parent->rb_right;
            if(rb_color(other)==RB_RED){
                rb_set_color(other, RB_BLACK);
                rb_set_color(parent, RB_RED);
                __rb_rotate_right(parent, root);
                other=parent->rb_left;
            }
            if((!other->rb_left || rb_color(other->rb_left)==RB_BLACK) &&
                (!other->rb_right || rb_color(other->rb_right)==RB_BLACK)){
                rb_set_color(other, RB_RED);
                node=parent;
                parent=rb_parent(node);
            }
            else{
                if(!other->rb_left || rb_color(other->rb_left)==RB_BLACK){
                    rb_set_color(other->rb_right, RB_BLACK);
                    rb_set_color(other, RB_RED);
                    __rb_rotate_left(other, root);
                    other=parent->rb_left;
                }
                rb_set_color(other, rb_color(parent));
                rb_set_color(parent, RB_BLACK);
                rb_set_color(other->rb_left,RB_BLACK);
                __rb_rotate_right(parent, root);
                node=root->rb_parent;
                break;
            }
        }
    }
    if(node)    rb_set_color(node, RB_BLACK);
}

/*Erase a node form the tree and trigger rebalancing
* Strategy: Due to node embedding, we cannot just copy data between node as non-intrusive rbtree might
* Instead, we find a substitue node and make it completely 
* replace the 'node'(usually as 'successor') to be erased within the tree structure
* so problem is then transformed:we now fix the hole 
* and potential r-b violations caused byy removing the successor form its origin position
*/
void rb_erase(rb_node_t *node, rb_root_t *root){
    rb_node_t *child, *parent;
    int color;
    /*
    * 'node' is what we want to delete
    * 'node_to_delete' is actually remove form the tree
    * 'child' is 'node_to_delete''s only child(can be NULL)
    */
    // part one: find the 'node_to_delete'
    rb_node_t *node_to_delete;
    rb_node_t *old=node;
    if(node->rb_left==NULL){
        child=node->rb_right;
    }else if(node->rb_right==NULL){
        child=node->rb_left;
    }else{
        //have two children, now 'successor' is minimun node in its right tree
        node_to_delete=node->rb_right;
        while(node_to_delete->rb_left!=NULL){
            node_to_delete=node_to_delete->rb_left;
        }   //now 'node_to_delete' is 'node''s successor
        //part two: find 'child'
        child=node_to_delete->rb_right;
        // Splicing
        parent=rb_parent(node_to_delete);
        color=rb_color(node_to_delete);
        if(parent){
            if(parent->rb_left==node_to_delete) parent->rb_left=child;
            else    parent->rb_right=child;
        }
        else    root->rb_parent=child;
        if(child)   rb_set_parent(child, parent);
        // 'Replace 'node'('old') with 'node_to_delete''
        node_to_delete->rb_parent_color=old->rb_parent_color;   //steels old's parent and color
        node_to_delete->rb_left=old->rb_left;
        rb_set_parent(old->rb_left, node_to_delete);
        node_to_delete->rb_right=old->rb_right;
        rb_set_parent(old->rb_right, node_to_delete);
        if(rb_parent(old)){
            if(rb_parent(old)->rb_left==old)    rb_parent(old)->rb_left=node_to_delete;
            else    rb_parent(old)->rb_right=node_to_delete;
            rb_set_parent(node_to_delete, rb_parent(old));
        }
        else    root->rb_parent=node_to_delete;
        goto start_fixup;
    }
    parent=rb_parent(node);
    color=rb_color(node);
    if(child)   rb_set_parent(child, parent);
    if(parent){
        if(parent->rb_left==node)   parent->rb_left=child;
        else    parent->rb_right=child;
    }
    else    root->rb_parent=child;
start_fixup:
    if(color==RB_BLACK) __rb_erase_color(child, parent, root);
}
void rb_link_node(rb_node_t *node, rb_node_t *parent, rb_node_t **rb_link){
    if(node==NULL || rb_link==NULL)   return;  
    node->rb_parent_color=(unsigned long)parent;
    node->rb_left=NULL;
    node->rb_right=NULL;
    *rb_link=node;
}