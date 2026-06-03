#include "namespace/context.h"

#include <bthread/bthread.h>
#include <bthread/types.h>

#include <mutex>

namespace EloqKV
{

static bthread_key_t ns_bthread_key;
static std::once_flag ns_bthread_key_once;
static bool bthread_key_initialized = false;

static void destroy_ns(void *ptr)
{
    delete static_cast<std::string *>(ptr);
}

static void InitializeBthreadNamespaceKey()
{
    std::call_once(ns_bthread_key_once,
                   []()
                   {
                       if (bthread_key_create(&ns_bthread_key, destroy_ns) == 0)
                       {
                           bthread_key_initialized = true;
                       }
                   });
}

std::string &GetCurrentNamespace()
{
    InitializeBthreadNamespaceKey();

    if (!bthread_key_initialized || bthread_self() == 0)
    {
        thread_local std::string fallback_ns = std::string(kDefaultNamespace);
        return fallback_ns;
    }

    void *ptr = bthread_getspecific(ns_bthread_key);
    if (ptr == nullptr)
    {
        std::string *ns_ptr = new std::string(kDefaultNamespace);
        bthread_setspecific(ns_bthread_key, ns_ptr);
        return *ns_ptr;
    }
    return *static_cast<std::string *>(ptr);
}

}  // namespace EloqKV
