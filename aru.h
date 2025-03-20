#ifndef ARU_H
#define ARU_H
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct aru aru;
typedef uint32_t aru_tag;

#define ARU_TAG_PENDING	(0)
#define ARU_TAG_DONE	(1)

/*
 * Returns pointer to an aru, or NULL on failure.
 */
struct aru *aru_init(void);

/*
 * Destory the given aru.
 */
void aru_destroy(struct aru *aru);

/*
 * aru_update - Update API provided to the user
 * @aru: pointer of the aru
 * @tag: status representing progress or result
 * @update: user's update function
 * @args: update function's arguments
 *
 * The user expects the update logic passed to this function to be executed
 * asynchronously, ensuring a critical section without interference from other
 * threads.
 *
 * The user can pass a tag as an argument to track the update status. If the tag
 * is NULL, no status will be provided.
 */
void aru_update(struct aru *aru, aru_tag *tag,
	void (*update)(void *args), void *args);

/*
 * aru_read - Read API provided to the user
 * @aru: pointer of the aru
 * @tag: status representing progress or result
 * @read: user's read function
 * @args: read function's arguments
 *
 * The user expects the read logic passed to this function to be executed
 * asynchronously. The read function will be executed after all previously
 * requested updates have been applied. Unlike the update logic, multiple read
 * operations can run concurrently, but update operations are not to be executed
 * simultaneously.
 *
 * The user can pass a tag as an argument to track the update status. If the tag
 * is NULL, no status will be provided.
 */
void aru_read(struct aru *aru, aru_tag *tag,
	void (*read)(void *args), void *args);

/*
 * aru_sync - Sync API provided to the user
 * @aru: pointer of the aru
 *
 * Explicitly execute the callback function for the given aru in the current
 * thread. For example, if the number of threads executing aru's callback
 * functions is lower than the number of read functions, this can be used to
 * improve read function throughput.
 */
void aru_sync(struct aru *aru);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* ARU_H */
