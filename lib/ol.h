// *O*skars *L*ibrary
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int64_t i64;
typedef uint64_t u64;
typedef int32_t i32;
typedef uint32_t u32;
typedef int16_t i16;
typedef uint16_t u16;
typedef int8_t i8;
typedef uint8_t u8;

typedef struct olStr olStr;
typedef struct olStrView olStrView;
typedef struct olStrBuilder olStrBuilder;
typedef struct olArena olArena;
typedef struct olArray olArray;
typedef struct olList olList;
typedef u32 olListNode;
typedef struct olMap olMap;
typedef struct olMapSlot olMapSlot;

// SECTION MACROS
#define null NULL
#define advance_ptr(ptr, by) (typeof(ptr))((u64)ptr + by)
#define ptr_diff(a, b) ((u64)a - (u64)b)

// SECTION STRINGS
struct olStr {
  u32 len;
  char data[];
};

// interns the string, len of 0 invokes strlen
#define olStr_fromConst(data) olStr_make(data, sizeof(data))
olStr *olStr_make(char *data, u32 len);
void olStr_delete(olStr *str);

struct olStrBuilder {
  u32 len;
  u32 cap;
  char *data;
};

olStrBuilder olStrBuilder_make(u32 reserve);
void olStrBuilder_appendc(olStrBuilder *sb, char c);
void olStrBuilder_appends(olStrBuilder *sb, char *c, u32 len);
void olStrBuilder_reserve(olStrBuilder *sb, u32 additional_cap);
olStr *olStrBuilder_finish(olStrBuilder *sb);
void olStrBuilder_delete(olStrBuilder *sb);

struct olStrView {
  u32 len;
  char *start;
};

#define olStrView_make(data, len_)                                             \
  (olStrView) { .start = data, .len = len_ }
olStrView olStrView_strips(olStr *str);
olStrView olStrView_strip(olStrView sv);

// SECTION ARENA
struct olArena {
  u64 cap;
  void *cur;
  void *data;
  void **last_section;
  u64 last_alloc_size;
};

olArena olArena_make(u64 size);
void *olArena_alloc(olArena *arena, u64 size);
void olArena_free_last(olArena *arena);
void olArena_reset(olArena *arena);
void olArena_push_section(olArena *arena);
void olArena_pop_section(olArena *arena);

// SECTION LIST
struct olArray {
  u32 cap;
  u32 used;
  u32 element_size;
  void *data;
};

#define olArray_of(type) olArray
#define olArray_makeof(type, element_count)                                    \
  olArray_make(sizeof(type), element_count)
#define olArray_pop(arr, type) (*(type *)_olArray_pop(arr))

olArray olArray_make(u32 element_size, u32 element_count);
void *olArray_push(olArray *list);
void *_olArray_pop(olArray *list);
void *olArray_get(olArray *list, u32 index);
void *olArray_getlast(olArray *list);
void olArray_delete(olArray *list);

// SECTION LINKEDLIST
struct olList {
  void *values;
  olListNode *next;
  olListNode *prev;
  u32 cap;
  u32 len;
  u32 element_size;
  olListNode last_node;
};

#define olList_of(type) olLinkedList
#define olList_makeof(type) olLinkedList_make(sizeof(type))

olList olList_make(u32 element_size);
void *olList_push(olList *ll);
void olList_pop(olList *ll);
olListNode olList_prev(olList *ll, olListNode n);
olListNode olList_next(olList *ll, olListNode n);
void *olList_val(olList *ll, olListNode n);
void olList_remove(olList *ll, olListNode node);
void olList_delete(olList *ll);

// SECTION MAP
struct olMapSlot {
  u64 hash;
  char data[];
};

struct olMap {
  olMapSlot *data;
  u32 cap;
  u32 used;
  u32 element_size;
};

#define olMap_of(type) olMap
#define olMap_makeof(type) olMap_make(sizeof(type))

olMap olMap_make(u32 element_size);
void *olMap_insert(olMap *map, olStr *key);
void olMap_remove(olMap *map, olStr *key);
void *olMap_get(olMap *map, olStr *key);
void *olMap_insertc(olMap *map, char *key, u32 len);
void olMap_removec(olMap *map, char *key, u32 len);
void *olMap_inserth(olMap *map, u64 hash);
void *olMap_getc(olMap *map, char *key, u32 len);
void *olMap_geth(olMap *map, u64 hash);
void olMap_delete(olMap *map);
u64 fnv1a(olStr *key);

// SECTION CONSOLE
#define log_debug(...) _log_(__FILE__, __LINE__, LOG_DEBUG, __VA_ARGS__)
#define log_info(...) _log_(__FILE__, __LINE__, LOG_INFO, __VA_ARGS__)
#define log_warn(...) _log_(__FILE__, __LINE__, LOG_WARN, __VA_ARGS__)
#define log_error(...) _log_(__FILE__, __LINE__, LOG_ERROR, __VA_ARGS__)
#define log_fatal(...) _log_(__FILE__, __LINE__, LOG_FATAL, __VA_ARGS__)

#define c_info(loc, ...) c_print_error((loc), LOG_INFO, __VA_ARGS__)
#define c_warn(loc, ...) c_print_error((loc), LOG_WARN, __VA_ARGS__)
#define c_error(loc, ...) c_print_error((loc), LOG_ERROR, __VA_ARGS__)

#define c_msg(loc, level, ...) c_print_error((loc), (level), __VA_ARGS__)

typedef enum {
  COLOR_GREY = 0,
  COLOR_GREEN,
  COLOR_CYAN,
  COLOR_YELLOW,
  COLOR_RED,
} colors_e;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARN,
  LOG_ERROR,
  LOG_FATAL,
} log_level_e;

void olLog_init();
void olLog_set_color(colors_e color);
void olLog_set_bold(void);
void olLog_reset_bold(void);
void olLog_set_underline(void);
void olLog_reset_underline(void);
void olLog_reset(void);
void olLog_print_time(void);
void _log_(char *file, int line, log_level_e level, char *fmt, ...);