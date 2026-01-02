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
void dummy_force_save_ra(void){
    asm volatile("nop");    //Solely to prevent being optimized away.
}
static void my_earse_color(rb_node_t * node, rb_node_t *parent, rb_root_t *root);
//All rotataion process the upper-layer structure first, then the sub-structure.
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
        gparent=rb_parent(parent);
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
* "Borrowing" is geometrically manifested as rotation and chromatically as recoloring. 
* By locating a red nephew node, we rotate it into the target path and turn it black. 
* Since that node was already red on the sibling's side, we have effectively "borrowed" the required black height
*/
static void __rb_erase_color(rb_node_t *node, rb_node_t *parent, rb_root_t *root){
    rb_node_t *other=NULL;  //slibing node
    //As long as 'node' is not the root and 'node' is black the loops continue
    while(node !=root->rb_parent && (node==NULL || rb_color(node)==RB_BLACK)){
        if(parent==NULL)    break;
        if(node==parent->rb_left){
            other=parent->rb_right;
            //We cannnot directly borrow black height from a red sibliing.
            if(rb_color(other)==RB_RED){//sibling 'other' is red,transform to black first!
                //current parent is black, sl(sibling left) and sr(sibling) is black.
                rb_set_color(other, RB_BLACK);
                rb_set_color(parent,RB_RED);
                //reshaping the tree.
                __rb_rotate_left(parent, root);
                other=parent->rb_right;
            }
            //The following scenarios all assume the sibling node is black.(able to borrow)
            //Black Sibliing, black nephews
            if((!other->rb_left || rb_color(other->rb_left)==RB_BLACK) &&
                (!other->rb_right || rb_color(other->rb_right)==RB_BLACK)){
                    //
                rb_set_color(other, RB_RED);
                node=parent;    //move the double-black up to the parent
                parent=rb_parent(node); //Continue loop from the parent
            }
            else{   //At least one nephew is red.
                //other's right is black, other's left is red(intermediate cases)
                if(!other->rb_right || rb_color(other->rb_right)==RB_BLACK){
                    rb_set_color(other->rb_left, RB_BLACK);
                    rb_set_color(other, RB_RED);
                    __rb_rotate_right(other, root);
                    other=parent->rb_right;
                }
                //(After adjust)now other's right is red
                rb_set_color(other, rb_color(parent));
                rb_set_color(parent, RB_BLACK);
                rb_set_color(other->rb_right, RB_BLACK);
                __rb_rotate_left(parent, root);
                node=root->rb_parent;break;
            }
        }
        else{
            other=parent->rb_left;
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
* so problem is then transformed: we now fix the hole 
* and potential r-b violations caused by removing the successor form its origin position
*/
void rb_erase(rb_node_t *node, rb_root_t *root){
    rb_node_t *node_child, *node_parent=rb_parent(node);
    int node_color=rb_color(node), del_node_color;
    rb_node_t *repl_node, *repl_parent;
    /*
    * 'node' is what we want to delete
    * 'successor' is the node effectively removed from its original place to replace 'node'
    * 'child' is 'successor''s only child (can be NULL)
    */

    // Case 1 & 2: Node has at most one child
    if(node->rb_left == NULL){
        node_child = node->rb_right;
    }else if(node->rb_right == NULL){
        node_child = node->rb_left;
    }else{
        // Case 3: Node has two children
        // We need to find the successor (minimum node in the right subtree)
        (void )node_child;
        rb_node_t *successor, *succ_child;
        successor = node->rb_right;
        while(successor->rb_left != NULL){
            successor = successor->rb_left;
        }
        
        // part two: find 'succ_child' (successor's right succ_child)
        succ_child = successor->rb_right;
        rb_node_t *succ_child_new_parent = rb_parent(successor);
        del_node_color = rb_color(successor); // Save successor's original color for fixup decision

        // Handle "Successor is immediate right succ_child" case
        // If successor == node->rb_right, we must NOT set successor->rb_right = node->rb_right (Self-Reference!)
        if(successor == node->rb_right) {
            // Special Case: Successor is the direct succ_child of node
            // The hole's parent becomes the successor itself
            succ_child_new_parent = successor; 
        } else {
            // Standard Case: Successor is further down the tree
            // Splicing: Remove successor from its original position
            if (succ_child_new_parent) succ_child_new_parent->rb_left = succ_child;
            rb_set_parent(succ_child, succ_child_new_parent);

            // Connect successor to node's right succ_child
            successor->rb_right = node->rb_right;
            rb_set_parent(node->rb_right, successor);
        }

        // 'Replace' 'node' with 'successor'
        // Successor inherits node's parent pointer AND color (to maintain local black-height)
        rb_set_color(successor, node_color);
        rb_set_parent(successor, node_parent);

        //have inhertis node's rb_right.
        successor->rb_left = node->rb_left;
        rb_set_parent(node->rb_left, successor); //left and right succ_child must exist!

        // Connect node's parent to successor
        if(node_parent){
            if(node_parent->rb_left == node) node_parent->rb_left = successor;
            else node_parent->rb_right = successor;
        } else {
            root->rb_parent = successor;
        }
        repl_node=succ_child;
        repl_parent=succ_child_new_parent;
        // Fixup starts from the hole left by successor
        goto start_fixup;
    }

    // Simple case handling (Node has 0 or 1 succ_child)    
    rb_set_parent(node_child, node_parent);
    if(node_parent){
        if(node_parent->rb_left == node) node_parent->rb_left = node_child;
        else 
            node_parent->rb_right = node_child;
    } else {
        root->rb_parent = node_child;
    }
    repl_node=node_child;
    repl_parent=node_parent;
    del_node_color=rb_color(node);
start_fixup:
    // Only trigger rebalancing if we removed a BLACK node
    // replace_node current is double black, it can be NULL
    if(del_node_color==RB_BLACK) my_earse_color(repl_node, repl_parent, root);
}


void rb_link_node(rb_node_t *node, rb_node_t *parent, rb_node_t **rb_link){
    if(node==NULL || rb_link==NULL)   return;  
    node->rb_parent_color=(unsigned long)parent;
    node->rb_left=NULL;
    node->rb_right=NULL;
    *rb_link=node;
}


static void my_earse_color(rb_node_t *node, rb_node_t *parent, rb_root_t *root){
    rb_node_t *other=NULL;
    while(node!=root->rb_parent && rb_color(node)==RB_BLACK){
        if(parent==NULL)    break;
        if(parent->rb_left==node){
            other=parent->rb_right;
            if(rb_color(other)==RB_RED){
                rb_set_color(other, rb_color(parent));
                rb_set_color(parent, RB_RED);
                __rb_rotate_left(parent, root);
                other=parent->rb_right;
            }
            if(rb_color(other->rb_left)==RB_BLACK 
                && rb_color(other->rb_right)==RB_BLACK){
                rb_set_color(other, RB_RED);
                node=parent;
                parent=rb_parent(node);
                continue;
            }
            else{
                if(rb_color(other->rb_right)==RB_BLACK){
                    //Current other->rb_left must be red
                    rb_set_color(other->rb_left, RB_BLACK);
                    rb_set_color(other, RB_RED);
                    __rb_rotate_right(other, root);
                    other=parent->rb_right;
                }
                rb_set_color(other, rb_color(parent));
                rb_set_color(parent, RB_BLACK);
                rb_set_color(other->rb_right, RB_BLACK);
                __rb_rotate_left(parent, root);
                return;
            }
        }
        else if(parent->rb_right==node){
            other=parent->rb_left;
            if(rb_color(other)==RB_RED){
                rb_set_color(other, rb_color(parent));  //must be black
                rb_set_color(parent, RB_RED);
                __rb_rotate_right(parent, root);
                other=parent->rb_left;
            }
            if(rb_color(other->rb_left)==RB_BLACK && rb_color(other->rb_right)==RB_BLACK){
                rb_set_color(other, RB_RED);
                node=parent;
                parent=rb_parent(node);
                continue;
            }
            else{
                if(rb_color(other->rb_left)==RB_BLACK){
                    rb_set_color(other->rb_right, RB_BLACK);
                    rb_set_color(other, RB_RED);
                    __rb_rotate_left(other, root);
                    other=parent->rb_left;
                }
                rb_set_color(other, rb_color(parent));
                rb_set_color(parent, RB_BLACK);
                rb_set_color(other->rb_left, RB_BLACK);
                __rb_rotate_right(parent, root);
                return;
            }
        }
        else{
            printf("Not the current node's parent, plz check again!\n");
            panic("My_erase_color!");
            return;
        }
    }
    rb_set_color(root->rb_parent, RB_BLACK);    //Maintain black root
}