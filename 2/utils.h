#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#pragma once

#define str_equal(str1, str2) strcmp((str1), (str2)) == 0

#define LIST_CAPACITY 4

void assert_true(bool condition, const char *error_message);

void *checked_realloc(void *ptr, size_t new_capacity);

typedef struct
{
    size_t size, capacity;
    void **items;
} list;

list *
list_init();

void
list_add(list *list_ptr, void *item);

void
list_trim(list *list_ptr);

#define list_free(list, item_cleaner)                                           \
do {                                                                            \
    for (size_t i = 0; i < (list)->capacity; ++i)                               \
        (item_cleaner)((list)->items[i]);                                       \
                                                                                \
    free((list)->items);                                                        \
    free((list));                                                               \
} while (false)

typedef struct
{
    size_t size;
    char *content;
} string_builder;

string_builder
*string_builder_init();

void
string_builder_append(string_builder *builder, char c);

char
*to_string(string_builder *builder);

void
string_builder_free(string_builder *builder);
