#include <stdlib.h>

void *hop_test_pool_malloc(size_t size);
#define malloc hop_test_pool_malloc
