/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010 Facebook, Inc. (http://www.facebook.com)          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <runtime/base/fiber_async_func.h>
#include <runtime/base/builtin_functions.h>
#include <runtime/base/resource_data.h>
#include <util/job_queue.h>
#include <util/lock.h>
#include <util/logger.h>

using namespace std;

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////
/**
 * This class provides synchronization between request thread and fiber thread
 * so to make sure when fiber job finishes after request is finished, which
 * means end_user_func_async() is forgotten, fiber job will not touch request
 * thread's data. There is no need to restore any states in this case.
 */
class FiberAsyncFuncData {
public:
  FiberAsyncFuncData() : m_reqId(0) {}
  Mutex m_mutex;
  int64 m_reqId;
};
static IMPLEMENT_THREAD_LOCAL(FiberAsyncFuncData, s_fiber_data);

void FiberAsyncFunc::OnRequestExit() {
  Lock lock(s_fiber_data->m_mutex);
  ++s_fiber_data->m_reqId;
};

///////////////////////////////////////////////////////////////////////////////

class FiberJob : public Synchronizable {
public:
  FiberJob(FiberAsyncFuncData *thread, CVarRef function, CArrRef params,
           bool async)
      : m_thread(thread),
        m_unmarshaled_function(NULL), m_unmarshaled_params(NULL),
        m_function(function), m_params(params), m_refCount(0),
        m_async(async), m_ready(false), m_done(false), m_delete(false),
        m_exit(false) {
    m_reqId = m_thread->m_reqId;

    // Profoundly needed: (1) to make sure references and objects are held
    // when job finishes, as otherwise, caller can release its last reference
    // to destruct them, then unmarshal coding will fail. (2) to make sure
    // references have refcount > 1, which is needed by
    // Variant::fiberUnmarshal() code to tell who needs to set back to original
    // reference.
    if (m_async) {
      m_unmarshaled_function = NEW(Variant)();
      *m_unmarshaled_function = m_function;
      m_unmarshaled_params = NEW(Variant)();
      *m_unmarshaled_params = m_params;
    }
  }

  ~FiberJob() {
  }

  void cleanup() {
    if (m_unmarshaled_function) {
      Lock lock(m_thread->m_mutex);
      if (m_thread->m_reqId == m_reqId) {
        DELETE(Variant)(m_unmarshaled_function);
        DELETE(Variant)(m_unmarshaled_params);
        m_unmarshaled_function = NULL;
        m_unmarshaled_params = NULL;
      }
      // else not safe to touch these members because thread has moved to
      // next request after deleting/collecting all these dangling ones
    }
  }

  void waitForReady() {
    Lock lock(this);
    while (!m_ready) wait();
  }

  bool isDone() {
    return m_done;
  }

  bool canDelete() {
    return m_delete && m_refCount == 1;
  }

  void run() {
    // make local copy of m_function and m_params
    if (m_async) {
      m_function = m_function.fiberMarshal(m_refMap);
      m_params = m_params.fiberMarshal(m_refMap);

      Lock lock(this);
      m_ready = true;
      notify();
    }

    try {
      m_return = f_call_user_func_array(m_function, m_params);
    } catch (const ExitException &e) {
      m_exit = true;
    } catch (const Exception &e) {
      m_fatal = String(e.getMessage());
    } catch (Object e) {
      m_exception = e;
    } catch (...) {
      m_fatal = String("unknown exception was thrown");
    }

    Lock lock(this);
    m_done = true;
    notify();
  }

  Variant syncGetResults() {
    if (m_exit) {
      throw ExitException(0);
    }
    if (!m_fatal.isNull()) {
      throw FatalErrorException("%s", m_fatal.data());
    }
    if (!m_exception.isNull()) {
      throw m_exception;
    }
    return m_return;
  }

  Variant getResults(FiberAsyncFunc::Strategy strategy, CVarRef resolver) {
    if (!m_async) return syncGetResults();

    {
      Lock lock(this);
      while (!m_done) wait();
    }

    // these are needed in case they have references or objects
    if (!m_refMap.empty()) {
      m_function.fiberUnmarshal(m_refMap);
      m_params.fiberUnmarshal(m_refMap);
    }

    Object unmarshaled_exception =
      m_exception.fiberUnmarshal(m_refMap);
    Variant unmarshaled_return =
      m_return.fiberUnmarshal(m_refMap);

    try {
      if (m_exit) {
        throw ExitException(0);
      }
      if (!m_fatal.isNull()) {
        throw FatalErrorException("%s", m_fatal.data());
      }
      if (!m_exception.isNull()) {
        throw unmarshaled_exception;
      }
    } catch (...) {
      cleanup();
      m_delete = true;
      throw;
    }

    cleanup();
    m_delete = true;
    return unmarshaled_return;
  }

  // ref counting
  void incRefCount() {
    Lock lock(m_mutex);
    ++m_refCount;
  }
  void decRefCount() {
    Lock lock(m_mutex);
    if (--m_refCount == 0) {
      delete this;
    }
  }

private:
  FiberAsyncFuncData *m_thread;

  // holding references to them, so we can later restore their states safely
  Variant *m_unmarshaled_function;
  Variant *m_unmarshaled_params;

  FiberReferenceMap m_refMap;
  int64 m_reqId;

  Variant m_function;
  Array m_params;

  Mutex m_mutex;
  int m_refCount;

  bool m_async;
  bool m_ready;
  bool m_done;
  bool m_delete;

  bool m_exit;
  String m_fatal;
  Object m_exception;
  Variant m_return;
};

///////////////////////////////////////////////////////////////////////////////

class FiberWorker : public JobQueueWorker<FiberJob*> {
public:
  ~FiberWorker() {
  }

  virtual void doJob(FiberJob *job) {
    job->run();
    m_jobs.push_back(job);
    cleanup();
  }

  void cleanup() {
    list<FiberJob*>::iterator iter = m_jobs.begin();
    while (iter != m_jobs.end()) {
      FiberJob *job = *iter;
      if (job->canDelete()) {
        job->decRefCount();
        iter = m_jobs.erase(iter);
        continue;
      }
      ++iter;
    }
  }

private:
  list<FiberJob*> m_jobs;
};

///////////////////////////////////////////////////////////////////////////////

class FiberAsyncFuncHandle : public ResourceData {
public:
  DECLARE_OBJECT_ALLOCATION(FiberAsyncFuncHandle);

  FiberAsyncFuncHandle(CVarRef function, CArrRef params, bool async) {
    m_job = new FiberJob(s_fiber_data.get(), function, params, async);
    m_job->incRefCount();
  }

  ~FiberAsyncFuncHandle() {
    m_job->decRefCount();
  }

  FiberJob *getJob() { return m_job;}

  // overriding ResourceData
  virtual const char *o_getClassName() const { return "FiberAsyncFuncHandle";}

private:
  FiberJob *m_job;
};

IMPLEMENT_OBJECT_ALLOCATION(FiberAsyncFuncHandle);

///////////////////////////////////////////////////////////////////////////////

static JobQueueDispatcher<FiberJob*, FiberWorker> *s_dispatcher;

void FiberAsyncFunc::Restart() {
  if (s_dispatcher) {
    s_dispatcher->stop();
    delete s_dispatcher;
    s_dispatcher = NULL;
  }
  if (RuntimeOption::FiberCount > 0) {
    s_dispatcher = new JobQueueDispatcher<FiberJob*, FiberWorker>
      (RuntimeOption::FiberCount, NULL);
    Logger::Info("fiber job dispatcher started");
    s_dispatcher->start();
  }
}

Object FiberAsyncFunc::Start(CVarRef function, CArrRef params) {
  FiberAsyncFuncHandle *handle =
    NEW(FiberAsyncFuncHandle)(function, params, s_dispatcher != NULL);
  Object ret(handle);

  FiberJob *job = handle->getJob();
  if (s_dispatcher) {
    job->incRefCount(); // paired with worker's decRefCount()
    s_dispatcher->enqueue(job);
    job->waitForReady(); // until job data are copied into fiber
  } else {
    job->run(); // immediately executing the job
  }

  return ret;
}

bool FiberAsyncFunc::Status(CObjRef func) {
  FiberAsyncFuncHandle *handle = func.getTyped<FiberAsyncFuncHandle>();
  return handle->getJob()->isDone();
}

Variant FiberAsyncFunc::Result(CObjRef func, Strategy strategy,
                               CVarRef resolver) {
  FiberAsyncFuncHandle *handle = func.getTyped<FiberAsyncFuncHandle>();
  return handle->getJob()->getResults(strategy, resolver);
}

///////////////////////////////////////////////////////////////////////////////
}
