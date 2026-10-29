#ifndef MEM_CLEAN_H
#define MEM_CLEAN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void mem_clean_init(void);
void mem_clean_shutdown(void);
void mem_clean_update(void);
void mem_clean_now(void);
void clean_courses_and_thumbnails(void);

#ifdef __cplusplus
}
#endif

#endif // MEM_CLEAN_H

