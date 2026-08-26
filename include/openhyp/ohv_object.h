/*
 * Handles the framework hands out are Objective-C objects, and callers free
 * them by calling os_release() on them -- which is objc_release(), which
 * reads a class pointer out of the first word of whatever it is given.  A
 * plain `new` returns memory whose first word is ours, so a caller doing the
 * ordinary thing with an ordinary handle takes a segmentation fault inside
 * the objc runtime, nowhere near this library.
 *
 * So the handles are real objects: instances of a class made at runtime with
 * NSObject as its superclass, which is what makes retain, release and the
 * dealloc that frees them all work by themselves.  The cost is one pointer at
 * the front of each handle, which every struct below declares.
 */
#ifndef OPENHYP_OHV_OBJECT_H
#define OPENHYP_OHV_OBJECT_H

#include <objc/runtime.h>
#include <stddef.h>

/*
 * Allocate a handle of `size` bytes whose first word is a usable isa.  The
 * memory comes back zeroed, as `new T{}` did.  Returns null only if the objc
 * runtime refuses to make the class, in which case the caller reports the
 * allocation failure it would have reported anyway.
 */
void *ohv_object_alloc(size_t size);

#endif
