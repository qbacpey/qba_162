#ifndef __LIB_KERNEL_LIST_H
#define __LIB_KERNEL_LIST_H

/* Doubly linked list.

   This implementation of a doubly linked list does not require
   use of dynamically allocated memory.  Instead, each structure
   that is a potential list element must embed a struct list_elem
   member.  All of the list functions operate on these `struct
   list_elem's.  The list_entry macro allows conversion from a
   struct list_elem back to a structure object that contains it.

   For example, suppose there is a needed for a list of `struct
   foo'.  `struct foo' should contain a `struct list_elem'
   member, like so:

      struct foo
        {
          struct list_elem elem;
          int bar;
          ...other members...
        };

   Then a list of `struct foo' can be be declared and initialized
   like so:

      struct list foo_list;

      list_init (&foo_list);

   Iteration is a typical situation where it is necessary to
   convert from a struct list_elem back to its enclosing
   structure.  Here's an example using foo_list:

      struct list_elem *e;

      for (e = list_begin (&foo_list); e != list_end (&foo_list);
           e = list_next (e))
        {
          struct foo *f = list_entry (e, struct foo, elem);
          ...do something with f...
        }

   You can find real examples of list usage throughout the
   source; for example, malloc.c, palloc.c, and thread.c in the
   threads directory all use lists.

   The interface for this list is inspired by the list<> template
   in the C++ STL.  If you're familiar with list<>, you should
   find this easy to use.  However, it should be emphasized that
   these lists do *no* type checking and can't do much other
   correctness checking.  If you screw up, it will bite you.

   Glossary of list terms:

     - "front": The first element in a list.  Undefined in an
       empty list.  Returned by list_front().

     - "back": The last element in a list.  Undefined in an empty
       list.  Returned by list_back().

     - "tail": The element figuratively just after the last
       element of a list.  Well defined even in an empty list.
       Returned by list_end().  Used as the end sentinel for an
       iteration from front to back.

     - "beginning": In a non-empty list, the front.  In an empty
       list, the tail.  Returned by list_begin().  Used as the
       starting point for an iteration from front to back.

     - "head": The element figuratively just before the first
       element of a list.  Well defined even in an empty list.
       Returned by list_rend().  Used as the end sentinel for an
       iteration from back to front.

     - "reverse beginning": In a non-empty list, the back.  In an
       empty list, the head.  Returned by list_rbegin().  Used as
       the starting point for an iteration from back to front.

     - "interior element": An element that is not the head or
       tail, that is, a real list element.  An empty list does
       not have any interior elements.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* List element. */
struct list_elem {
  struct list_elem* prev; /* Previous list element. */
  struct list_elem* next; /* Next list element. */
};

/* List. */
struct list {
  struct list_elem head; /* List head. */
  struct list_elem tail; /* List tail. */
};

/* Converts pointer to list element LIST_ELEM into a pointer to
   the structure that LIST_ELEM is embedded inside.  Supply the
   name of the outer structure STRUCT and the member name MEMBER
   of the list element.  See the big comment at the top of the
   file for an example. */
#define list_entry(LIST_ELEM, STRUCT, MEMBER)                                                      \
  ((STRUCT*)((uint8_t*)&(LIST_ELEM)->next - offsetof(STRUCT, MEMBER.next)))

/** 
* list_for_each_entry  -       iterate over list of given type 
* 注意，word_count里边的elem是一个结构体，而不是一个指向list_elem的指针
* 同时，整个操作过程中不可移除其中元素
* TODO 这里有个问题，pos是会将尾哨兵当成链表元素的
* @pos:        the type * to use as a loop counter. 
* @list:       the pointer of list. 
* @member:     the name of the list_struct within the struct. 
 */
#define list_for_each_entry(pos, list, member)                                                     \
  for (pos = list_entry(list_begin(list), typeof(*pos), member);    \
       &(pos->member) != list_end(list); pos = list_entry(pos->member.next, typeof(*pos), member))

/** 
* list_clean_each  -       iterate over list of given type 
* 注意，word_count里边的elem是一个结构体，而不是一个指向list_elem的指针
* 不同之处在于这个是用于清除列表元素的（没有回头路）
* 两点要求：
* 1. pos 初始必须设置为 NULL
* 2. for循环后必须添加if 
* @pos:        the type * to use as a loop counter. 
* @list:       the pointer of list. 
* @member:     the name of the list_struct within the struct. 
* @action:     需要对列表中每一个元素执行的清理相关的工作
 */
#define list_clean_each(pos, list, member, action)                                                 \
  do {                                                                                             \
    for (pos = !list_empty(list) ? list_entry(list_pop_front(list), typeof(*pos), member) : NULL;  \
         !list_empty(list); pos = list_entry(list_pop_front(list), typeof(*pos), member)) {        \
      action;                                                                                      \
    }                                                                                              \
    if (pos != NULL) {                                                                             \
      action;                                                                                      \
    }                                                                                              \
  } while (0) // 尾部的if语句是必要的

/* List initialization.

   A list may be initialized by calling list_init():

       struct list my_list;
       list_init (&my_list);

   or with an initializer using LIST_INITIALIZER:

       struct list my_list = LIST_INITIALIZER (my_list); */
#define LIST_INITIALIZER(NAME)                                                                     \
  {                                                                                                \
    {NULL, &(NAME).tail}, { &(NAME).head, NULL }                                                   \
  }

void list_init(struct list*);

/* List traversal. */
struct list_elem* list_begin(struct list*);
struct list_elem* list_next(struct list_elem*);
struct list_elem* list_end(struct list*);

struct list_elem* list_rbegin(struct list*);
struct list_elem* list_prev(struct list_elem*);
struct list_elem* list_rend(struct list*);

struct list_elem* list_head(struct list*);
struct list_elem* list_tail(struct list*);

/* List insertion. */
void list_insert(struct list_elem*, struct list_elem*);
void list_splice(struct list_elem* before, struct list_elem* first, struct list_elem* last);
void list_push_front(struct list*, struct list_elem*);
void list_push_back(struct list*, struct list_elem*);

/* List removal. */
struct list_elem* list_remove(struct list_elem*);
struct list_elem* list_pop_front(struct list*);
struct list_elem* list_pop_back(struct list*);

/* List elements. */
struct list_elem* list_front(struct list*);
struct list_elem* list_back(struct list*);

/* List properties. */
size_t list_size(struct list*);
bool list_empty(struct list*);

/* Miscellaneous. */
void list_reverse(struct list*);

/**
  * Compares the value of two list elements A and B, given
  * auxiliary data AUX.  Returns true if A is less than B, or
  * false if A is greater than or equal to B. 
  * 
  * 比较列表元素a和b：
  * 如果a应该放在b的前面，那么返回true
  * 如果a应该放在b的后面，那么返回false
  * 具体行为将a和b解包成链表外包装，然后使用aux比较它们
  * 
  * aux则最好不要这样理解，aux是实际接收并比较列表外包装的函数
  * 如果a比b小，那么返回true
  * 如果a比b大，那么返回true
  * 
  * 总的来说，list_less_func的语义偏向于a和b位置关系
  * aux的语义偏向于a和b的大小关系，是真正比较a和b需要使用的函数
  * 
  * 举例来说，如果需要使用这套系统维护线程优先级列表
  * 并且这个列表满足不变性：优先级最大的在最前面、新元素需要插到同等优先级元素的最后
  * 那么就可以将list_less_func类型的函数命名为：list_name_before
  * 将aux命名为：grater_equal_thread_pri
  * 而如果是插到同等元素的最前面，那么可以将aux命名为：grater_thread_pri
  * 
  */
typedef bool list_less_func(const struct list_elem* a, const struct list_elem* b, void* aux);

/* Operations on lists with ordered elements. */
void list_sort(struct list*, list_less_func*, void* aux);
void list_insert_ordered(struct list*, struct list_elem*, list_less_func*, void* aux);
void list_unique(struct list*, struct list* duplicates, list_less_func*, void* aux);

/* Max and min. */
struct list_elem* list_max(struct list*, list_less_func*, void* aux);
struct list_elem* list_min(struct list*, list_less_func*, void* aux);

#endif /* lib/kernel/list.h */
