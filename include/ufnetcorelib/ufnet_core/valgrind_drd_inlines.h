/*
 * valgrind_drd_inlines.h
 *
 *  Created on: 25 Jul 2016
 *      Author: ayman
 */

#ifndef SRC_INCLUDE_VALGRIND_DRD_INLINES_H_
#define SRC_INCLUDE_VALGRIND_DRD_INLINES_H_


static inline void __vdrd_AnnotateIgnoreReadsBegin() {
#if defined(HAVE_VALGRIND_DRD)
	ANNOTATE_IGNORE_READS_BEGIN();
#endif
}
static inline void __vdrd_AnnotateIgnoreReadsEnd() {
#if defined(HAVE_VALGRIND_DRD)
	ANNOTATE_IGNORE_READS_END();
#endif
}

static inline void __vdrd_AnnotateIgnoreVariable(const volatile void *_ptr) {
#if defined(HAVE_VALGRIND_DRD)
	DRD_IGNORE_VAR(_ptr);
#endif
}


#endif /* SRC_INCLUDE_VALGRIND_DRD_INLINES_H_ */
