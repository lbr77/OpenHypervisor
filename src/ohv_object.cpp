#include "openhyp/ohv_object.h"

#include <objc/objc.h>
#include <pthread.h>

static Class g_handle_class;

static void make_class(void) {
    Class root = objc_getClass("NSObject");

    if (!root) {
        return;
    }
    /*
     * No ivars are added: what the handle holds sits in the extra bytes
     * class_createInstance is asked for, right behind the isa, which is what
     * lets each handle keep the layout it already had with one pointer in
     * front of it.
     */
    g_handle_class = objc_allocateClassPair(root, "OHVHandle", 0);
    if (!g_handle_class) {
        /* Another copy of the library in this process registered it first. */
        g_handle_class = objc_getClass("OHVHandle");
        return;
    }
    objc_registerClassPair(g_handle_class);
}

void *ohv_object_alloc(size_t size) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    size_t base;

    pthread_once(&once, make_class);
    if (!g_handle_class) {
        return nullptr;
    }
    base = class_getInstanceSize(g_handle_class);
    /*
     * class_createInstance zeroes what it allocates and sets the isa, so the
     * caller gets what `new T{}` gave it plus a class pointer at the front.
     */
    return (void *)class_createInstance(g_handle_class,
                                        size > base ? size - base : 0);
}
