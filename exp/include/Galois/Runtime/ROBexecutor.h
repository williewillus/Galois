/** Speculative Ordered Executor -*- C++ -*-
 * @file
 * This is the only file to include for basic Galois functionality.
 *
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE. NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */
#ifndef GALOIS_RUNTIME_ROB_EXECUTOR_H
#define GALOIS_RUNTIME_ROB_EXECUTOR_H

#include "Galois/Accumulator.h"
#include "Galois/Atomic.h"
#include "Galois/BoundedVector.h"
#include "Galois/Galois.h"
#include "Galois/gdeque.h"
#include "Galois/PriorityQueue.h"
#include "Galois/Timer.h"

#include "Galois/Runtime/Barrier.h"
#include "Galois/Runtime/Context.h"
#include "Galois/Runtime/DoAll.h"
#include "Galois/Runtime/ForEachTraits.h"
#include "Galois/Runtime/ParallelWork.h"
#include "Galois/Runtime/PerThreadWorkList.h"
#include "Galois/Runtime/Range.h"
#include "Galois/Runtime/Sampling.h"
#include "Galois/Runtime/Support.h"
#include "Galois/Runtime/Termination.h"
#include "Galois/Runtime/ThreadPool.h"
#include "Galois/Runtime/UserContextAccess.h"
#include "Galois/Runtime/ll/gio.h"
#include "Galois/Runtime/ll/ThreadRWlock.h"
#include "Galois/Runtime/ll/CompilerSpecific.h"
//#include "Galois/Runtime/ll/PthreadLock.h"
#include "Galois/Runtime/mm/Mem.h"

#include <iostream>

namespace Galois {
namespace Runtime {

  // race conditions
  // 1. two iterations trying to abort the same iteration
  //  a. two iterations trying to abort an iteration that has already executed
  //  b. an iteration trying to abort self, while other aborting it when clearing
  //  rob
  // 2. The iteration itself trying to go into RTC, while other trying to abort it
  // 3. Two threads trying to schedule item from pending
  // 4. One thread trying to abort or add an item after commit, while other trying to
  // schedule an item from pending
  // 5. 

  // multiple attempts to abort an iteration
  // soln1: use a mutex per iteration and use state to indicate if someone else
  // already aborted the iteration
  // soln2: for an iteration that has executed, the threads competing to abort it
  // use a cas (on state) to find the winner who goes on to abort the iteration
  // for an iteration that has not completed execution yet, the thread signals the
  // iteration to abort itself. each iteration keeps track of its owner thread and
  // only the owner thread aborts the iteration. 
  //

  // TODO: throw abort exception vs use a flag 
  // on self aborts
  // TODO: memory management: other threads may refer to an iteration context that has
  // been deallocated after commit or abort

namespace dbg {
  template <typename... Args>
  void debug (Args&&... args) {
    
    const bool DEBUG = false;
    if (DEBUG) {
      LL::gDebug (std::forward<Args> (args)...);
    }
  }
}

template <typename T, typename Cmp, typename Exec>
class ROBcontext: public SimpleRuntimeContext {

  using Base = SimpleRuntimeContext;
  using NhoodList =  Galois::gdeque<Lockable*, 4>; // 2nd arg is chunk size

public:

  enum State {
    UNSCHEDULED,
    SCHEDULED,
    READY_TO_COMMIT,
    ABORT_SELF,
    ABORT_HELP,
    COMMITTING,
    ABORTING,
    COMMIT_DONE,
    ABORT_DONE,
  };

  // TODO: privatize
public:
  GALOIS_ATTRIBUTE_ALIGN_CACHE_LINE Galois::GAtomic<State> state;
  T active;
  Exec& executor;

  bool lostConflict;
  volatile bool executed;

  unsigned owner;

  NhoodList nhood;
  UserContextAccess<T> userHandle;



private:
  ROBcontext (const ROBcontext& that) {} // TODO: remove since SimpleRuntimeContext inherits from boost::noncopyable

public:

  explicit ROBcontext (const T& x, Exec& e)
    : 
      Base (true), 
      state (UNSCHEDULED), 
      active (x), 
      executor (e), 
      lostConflict (false),
      executed (false),
      owner (LL::getTID ())

  {}

  bool hasExecuted () const { return executed; }

  bool hasState (State s) const { return ((State) state) == s; } 

  void setState (State s) { 
    state = s;
  }

  bool casState (State s_old, State s_new) { 
    return state.cas (s_old, s_new);
  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE virtual void subAcquire (Lockable* l) {

    for (bool succ = false; !succ; ) {
      Base::AcquireStatus acq = Base::tryAcquire (l);

      switch (acq) {
        case Base::FAIL: {
          ROBcontext* volatile that = static_cast<ROBcontext*> (Base::getOwner (l));
          if (that != nullptr) {
            // assert (dynamic_cast<ROBcontext*> (Base::getOwner (l)) != nullptr);
            bool abortSelf = resolveConflict (that, l);
            succ = abortSelf;
            lostConflict = true;

          } else {
            dbg::debug ("owner found to be null, current value: ", Base::getOwner (l)
                , " for lock: ", l);
          }
          break;
        }

        case Base::NEW_OWNER: {
          nhood.push_back (l);
          succ = true;
          break;
        }

        case Base::ALREADY_OWNER: {
          assert (std::find (nhood.begin (), nhood.end (), l) != nhood.end ());
          succ = true;
          break;
        }

        default: {
          GALOIS_DIE ("invalid acquire status");
          break;
        }
      }
    }
  }



  GALOIS_ATTRIBUTE_PROF_NOINLINE void doCommit () {
    assert (hasState (COMMITTING));
    // release locks
    // add new elements to worklist

    // TODO: check for typetraits 'noadd'

    userHandle.commit ();
    releaseLocks ();
    executor.push (userHandle.getPushBuffer ().begin (), userHandle.getPushBuffer ().end ());
    userHandle.reset ();

    LL::compilerBarrier ();

    setState (COMMIT_DONE);
  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE void doAbort () {
    assert (hasState (ABORTING));
    // perform undo actions in reverse order
    // release locks
    // add active element to worklist

    userHandle.rollback ();
    releaseLocks ();
    executor.push_abort (active, owner);
    userHandle.reset ();

    LL::compilerBarrier ();

    setState (ABORT_DONE);

  }

  struct PtrComparator {
    const Cmp& cmp;

    explicit PtrComparator (const Cmp& cmp): cmp (cmp) {}

    inline bool operator () (const ROBcontext* left, const ROBcontext* right) const {
      assert (left != nullptr);
      assert (right != nullptr);

      return cmp (left->active, right->active);
    }
  };

private:

  void releaseLocks () {
    for (Lockable* l: nhood) {
      assert (l != nullptr);
      if (Base::getOwner (l) == this) {
        dbg::debug (this, " releasing lock ", l);
        Base::release (l);
      }
    }

  }

private:

  GALOIS_ATTRIBUTE_PROF_NOINLINE bool resolveConflict (ROBcontext* volatile that, const Lockable* const l) {
    // precond: this could not acquire lock. lock owned by that
    // this can only be in state SCHEDULED or ABORT_SELF
    // that can be in SCHEDULED, ABORT_SELF, ABORT_HELP, READY_TO_COMMIT, ABORT_DONE
    // returns true if this loses the conflict and signals itself to abort
    
    bool ret = false;
    
    if (executor.getCtxtCmp () (this, that)) {

      assert (!that->hasState (COMMIT_DONE) && !that->hasState (COMMITTING));
      // abort that
      if (that->hasState (ABORT_DONE)) {
        // do nothing

      } else if (that->casState (SCHEDULED, ABORT_SELF) || that->hasState (ABORT_SELF)) {
        // signalled successfully
        // now wait for it to abort or abort yourself if 'that' missed the signal 
        // and completed execution
        dbg::debug ( this, " signalled ", that, " to ABORT_SELF on lock ", l);
        while (true) {

          if (that->hasState (ABORT_DONE)) {
            break;
          }

          if (that->hasExecuted ()) {
            if (that->casState (ABORT_SELF, ABORT_HELP)) {
              that->setState (ABORTING);
              that->doAbort ();
              executor.abortByOther += 1;
              dbg::debug (this, " aborting ABORT_SELF->ABORT_HELP missed signal ", that, " on lock ", l);
            }
                
          }

          LL::asmPause ();
        }

      } else if (that->casState (READY_TO_COMMIT, ABORT_HELP)) {
        that->setState (ABORTING);
        that->doAbort ();
        executor.abortByOther += 1;
        dbg::debug (this, " aborting RTC->ABORT_HELP ", that, " on lock ", l);
      }

    } else { 
      // abort self
      this->setState (ABORT_SELF);
      dbg::debug (this, " losing conflict with ", that, " on lock ", l);
      ret = true;
    }

    return ret;

  }



};

template <typename T, typename Cmp, typename NhFunc, typename OpFunc>
class ROBexecutor: private boost::noncopyable {

  using Ctxt = ROBcontext<T, Cmp, ROBexecutor>;
  using CtxtAlloc = Galois::Runtime::MM::FSBGaloisAllocator<Ctxt>;
  using CtxtCmp = typename Ctxt::PtrComparator;
  using CtxtDeq = Galois::Runtime::PerThreadDeque<Ctxt*>;
  using CtxtVec = Galois::Runtime::PerThreadVector<Ctxt*>;

  using PendingQ = Galois::MinHeap<T, Cmp>;
  using PerThrdPendingQ = PerThreadMinHeap<T, Cmp>;
  using ROB = Galois::MinHeap<Ctxt*, typename Ctxt::PtrComparator>;

  using Lock_ty = Galois::Runtime::LL::SimpleLock;
  // using Lock_ty = Galois::Runtime::LL::PthreadLock<true>;


  Cmp itemCmp;
  NhFunc nhFunc;
  OpFunc opFunc;
  CtxtCmp ctxtCmp;

  PerThrdPendingQ pending;
  ROB rob;
  TerminationDetection& term;


  CtxtAlloc ctxtAlloc;
  // CtxtDeq ctxtDelQ;
  CtxtDeq freeList;

  // GALOIS_ATTRIBUTE_ALIGN_CACHE_LINE Lock_ty pendingMutex;
  PerThreadStorage<Lock_ty> pendingMutex;

  GALOIS_ATTRIBUTE_ALIGN_CACHE_LINE Lock_ty robMutex;

  GAccumulator<size_t> numTotal;
  GAccumulator<size_t> numCommitted;
  GAccumulator<size_t> numGlobalCleanups;



  static const size_t WINDOW_SIZE_PER_THREAD = 1024;

  // static const size_t DELQ_THRESHOLD_UPPER = 1100;
  // static const size_t DELQ_THRESHOLD_LOWER = 1000;
  
public:

  GAccumulator<size_t> abortSelfByConflict;
  GAccumulator<size_t> abortSelfBySignal;
  GAccumulator<size_t> abortByOther;

  ROBexecutor (const Cmp& cmp, const NhFunc& nhFunc, const OpFunc& opFunc)
    : 
      itemCmp (cmp), 
      nhFunc (nhFunc), 
      opFunc (opFunc), 
      ctxtCmp (itemCmp),
      pending (itemCmp),
      rob (ctxtCmp), 
      term (getSystemTermination ())
  {}

  const Cmp& getItemCmp () const { return itemCmp; }

  const CtxtCmp& getCtxtCmp () const { return ctxtCmp; }

  template <typename Iter>
  void push (Iter beg, Iter end) {
    // TODO: whether to add new elements to the owner or to the committer?

    pendingMutex.getLocal ()->lock (); {
      for (Iter i = beg; i != end; ++i) {
        pending.get ().push (*i);
      }
    } 
    pendingMutex.getLocal ()->unlock ();
  }

  template <typename Iter>
  GALOIS_ATTRIBUTE_PROF_NOINLINE void push_initial (Iter beg, Iter end) {

    assert (beg != end);

    pending[0].push (*beg);
    ++beg;

    if (beg != end) {
      Galois::do_all (beg, end,
          [this] (const T& x) {
          pending.get ().push (x);
          });
    }

    assert (!pending.empty_all ());

    const T& dummy = *pending[0].begin ();

    Galois::on_each (
        [&dummy,this] (const unsigned tid, const unsigned numT) {
          for (unsigned j = 0; j < WINDOW_SIZE_PER_THREAD; ++j) {
            Ctxt* ctx = ctxtAlloc.allocate (1);
            assert (ctx != nullptr);
            ctxtAlloc.construct (ctx, dummy, *this);
            ctx->setState (Ctxt::SCHEDULED);

            freeList[tid].push_back (ctx);
          }
        });

    // for (unsigned i = 0; i < freeList.numRows (); ++i) {
      // for (unsigned j = 0; j < WINDOW_SIZE_PER_THREAD; ++j) {
        // Ctxt* ctx = ctxtAlloc.allocate (1);
        // assert (ctx != nullptr);
        // ctxtAlloc.construct (ctx, dummy, *this);
        // ctx->setState (Ctxt::SCHEDULED);
// 
        // freeList[i].push_back (ctx);
      // }
    // }
  }

  void push_abort (const T& x, const unsigned owner) {

    // TODO: who gets the aborted item, owner or aborter?

    // tree based serialization
    unsigned nextOwner = owner / 2;

    pendingMutex.getRemote (nextOwner)->lock (); {
      pending[nextOwner].push (x);
    } pendingMutex.getRemote (nextOwner)->unlock ();


    // pendingMutex.getLocal ()->lock (); {
      // pending.get ().push (x);
    // } pendingMutex.getLocal ()->unlock ();

  }

  void execute () {

    term.initializeThread ();

    do {

      bool didWork = false;

      do { 

        Ctxt* ctx = scheduleGlobalMinFirst ();

        if (ctx != nullptr) {

          didWork = true;

          dbg::debug (ctx, " scheduled with item ", ctx->active,
              " remaining contexts: ", freeList.get ().size ());

          applyOperator (ctx);

          if (!ctx->casState (Ctxt::SCHEDULED, Ctxt::READY_TO_COMMIT)) {
            if (ctx->casState (Ctxt::ABORT_SELF, Ctxt::ABORTING)) {

              if (ctx->lostConflict) {
                abortSelfByConflict += 1;

              } else {
                abortSelfBySignal += 1;
              }

              ctx->doAbort ();
              dbg::debug (ctx, " aborting SELF after reading signal");
            }
          }

          ctx->executed = true;

          LL::compilerBarrier ();

        }


        bool cleared = clearROB (ctx);

        didWork = didWork || cleared;

        // // TODO: termination detection
        // if (robEmpty) {
        // bool fin = false;
        // 
        // // check my queue first
        // pendingMutex.lock (); {
        // fin = pending.empty ();
        // } pendingMutex.unlock ();
        // 
        // abortQmutex.lock (); {
        // fin = fin && abortQ.empty ();
        // } abortQmutex.unlock ();
        // 
        // if (fin) {
        // break;
        // }
        // }

        // XXX: unprotected check. may crash
      } while (!rob.empty () || !pending.empty_all ());

      term.localTermination (didWork);
      
    } while (term.globalTermination ());
  }

  void operator () () {
    execute ();
  }

  void printStats () {
    // just to compile the size methods for debugging
    rob.size ();
    pending.size_all ();

    assert (rob.empty ());
    assert (pending.empty_all ());


    std::cout << "Total Iterations: " << numTotal.reduce () << std::endl;
    std::cout << "Number Committed: " << numCommitted.reduce () << std::endl;
    double ar = double (numTotal.reduce () - numCommitted.reduce ()) / double (numTotal.reduce ());

    std::cout << "Abort Ratio: " << ar << std::endl;

    double totalAborts = double (abortSelfByConflict.reduce () + abortSelfBySignal.reduce () + abortByOther.reduce ());

    std::cout << "abortSelfByConflict: " << abortSelfByConflict.reduce () << ", " << double (100*abortSelfByConflict.reduce ())/totalAborts << "%" << std::endl;
    std::cout << "abortSelfBySignal: " << abortSelfBySignal.reduce () << ", " << double (100*abortSelfBySignal.reduce ())/totalAborts << "%" << std::endl;
    std::cout << "abortByOther: " << abortByOther.reduce () << ", " << double (100*abortByOther.reduce ())/totalAborts << "%" << std::endl;

    std::cout << "Number of Global Cleanups: " << numGlobalCleanups.reduce () << std::endl;

  }

private:

  GALOIS_ATTRIBUTE_PROF_NOINLINE void applyOperator (Ctxt* ctx) {

    assert (ctx != nullptr);

    Galois::Runtime::setThreadContext (ctx);
    nhFunc (ctx->active, ctx->userHandle);

    if (ctx->hasState (Ctxt::SCHEDULED)) {
      opFunc (ctx->active, ctx->userHandle);
    }
    Galois::Runtime::setThreadContext (nullptr);

  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE Ctxt* scheduleGlobalMinFirst () {
    bool notEmpty = true;
    Ctxt* ctx = nullptr;

    // XXX: unprotected check, may cause crash
    notEmpty = !freeList.get ().empty () && !pending.empty_all ();

    if (notEmpty) {
      robMutex.lock (); {

        if (!freeList.get ().empty ()) {

          unsigned minTID = 0;
          const T* minPtr = nullptr;

          for (unsigned i = 0; i < getActiveThreads (); ++i) {

            pendingMutex.getRemote (i)->lock (); {

              if (!pending[i].empty ()) {
                if (minPtr == nullptr || itemCmp (pending[i].top (), *minPtr)) {
                  // minPtr == nullptr or pending[i].top () < *minPtr
                  minPtr = &pending[i].top ();
                  minTID = i;
                }
              }

            } pendingMutex.getRemote (i)->unlock ();
          } // end for

          pendingMutex.getRemote (minTID)->lock (); {

            if (!pending[minTID].empty ()) {

              ctx = freeList.get ().back ();
              freeList.get ().pop_back ();

              ctx->~Ctxt (); // destroy here only
              new (ctx) Ctxt (pending[minTID].pop (), *this);

              ctx->setState (Ctxt::SCHEDULED);
              ctx->owner = LL::getTID ();
              rob.push (ctx);
              numTotal += 1;
            }

          } pendingMutex.getRemote (minTID)->unlock ();

        }

      } robMutex.unlock ();

    } // end if notEmpty

    return ctx;
  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE Ctxt* scheduleThreadLocalFirst () {
    bool notEmpty = true;
    Ctxt* ctx = nullptr;

    // XXX: unprotected check, may cause crash
    notEmpty = !freeList.get ().empty () && !pending.empty_all ();

    if (notEmpty) {
      robMutex.lock (); {

        if (!freeList.get ().empty ()) {
          unsigned beg = LL::getTID ();
          unsigned end = beg + getActiveThreads ();

          for (unsigned i = beg; i < end; ++i) {

            unsigned tid = i % getActiveThreads ();

            pendingMutex.getRemote (tid)->lock (); {

              if (!pending[tid].empty ()) {
                ctx = freeList.get ().back ();
                freeList.get ().pop_back ();

                ctx->~Ctxt (); // destroy here only
                new (ctx) Ctxt (pending[tid].pop (), *this);

                ctx->setState (Ctxt::SCHEDULED);
                ctx->owner = LL::getTID ();
                rob.push (ctx);
                numTotal += 1;
              }

            } pendingMutex.getRemote (tid)->unlock ();

            if (ctx != nullptr) { 
              break;
            }

          } // end for
        } // end if freeList
      } robMutex.unlock ();
    }

    return ctx;
  }

  bool isEarliest (const T& x) {
    bool earliest = true;

    for (unsigned i = 0; i < getActiveThreads (); ++i) {

      pendingMutex.getRemote (i)->lock (); {

        if (!pending[i].empty ()) {
          earliest = earliest && !itemCmp (pending[i].top (), x);
        }

      } pendingMutex.getRemote (i)->unlock ();

      if (!earliest) { 
        break;
      }
    }

    return earliest;
  }

  GALOIS_ATTRIBUTE_PROF_NOINLINE bool clearROB (Ctxt* __ctxt) {
    bool didWork = false;

    robMutex.lock (); {
      while (!rob.empty ()) {

        Ctxt* head = rob.top ();

        if (head->hasState (Ctxt::ABORT_DONE)) {
          // ctxtDelQ.get ().push_back (rob.pop ());
          reclaim (rob.pop ());
          didWork = true;
          continue;

        }  else if (head->hasState (Ctxt::READY_TO_COMMIT)) {

          if (isEarliest (head->active)) {

            head->setState (Ctxt::COMMITTING);
            head->doCommit ();

            Ctxt* t = rob.pop ();
            assert (t == head);

            // ctxtDelQ.get ().push_back (t);
            reclaim (t);
            didWork = true;
            numCommitted += 1;
            dbg::debug (__ctxt, " committed: ", head, ", with active: ", head->active);

          } else {
            break;
          }

        } else {
          break;
        }
      }

      if (!rob.empty () && freeList.empty_all ()) {
        // a deadlock situation where no more contexts to schedule
        // and more work needs to be done
        reclaimGlobally ();
      }

    } robMutex.unlock ();

    return didWork;
  }

  void reclaim (Ctxt* ctx) {
    // assumes that robMutex is acquired
    unsigned owner = ctx->owner;
    // return to owner. Safe since we already hold robMutex and any scheduling
    // thread must also acquire robMutex.
    freeList[owner].push_back (ctx);
    // freeList.get ().push_back (ctx);
  }

  void reclaimGlobally () {
    // assumes that robMutex is acquired
    //

    numGlobalCleanups += 1;
    
    std::vector<Ctxt*> buffer;
    buffer.reserve (rob.size ());

    while (!rob.empty ()) {
      Ctxt* ctx = rob.pop ();

      if (ctx->hasState (Ctxt::ABORT_DONE)) {
        reclaim (ctx);

      } else {
        buffer.push_back (ctx);
      }
    }

    for (auto i = buffer.begin (), endi = buffer.end (); i != endi; ++i) {
      rob.push (*i);
    }

  }

 
  // void reclaimMemoryImpl (size_t upperLim, size_t lowerLim) {
    // assert (upperLim >= lowerLim);
// 
    // if (ctxtDelQ.get ().size () >= upperLim) {
      // for (size_t s = ctxtDelQ.get ().size (); s > lowerLim; --s) {
        // Ctxt* c = ctxtDelQ.get ().front ();
        // ctxtDelQ.get ().pop_front ();
// 
        // ctxtAlloc.destroy (c);
        // ctxtAlloc.deallocate (c, 1);
      // }
      // 
    // }
  // }
// 
  // void reclaimMemoryPeriodic () {
   // reclaimMemoryImpl (DELQ_THRESHOLD_UPPER, DELQ_THRESHOLD_LOWER);
  // }
// 
  // void reclaimMemoryFinal () {
    // reclaimMemoryImpl (0, 0);
  // }


};


template <typename Iter, typename Cmp, typename NhFunc, typename OpFunc>
void for_each_ordered_rob (Iter beg, Iter end, Cmp cmp, NhFunc nhFunc, OpFunc opFunc, const char* loopname=0) {

  using T = typename std::iterator_traits<Iter>::value_type;

  Galois::Runtime::beginSampling ();

  ROBexecutor<T, Cmp, NhFunc, OpFunc>  exec (cmp, nhFunc, opFunc);

  exec.push_initial (beg, end);

  getSystemThreadPool ().run (activeThreads, std::ref(exec));

  Galois::Runtime::endSampling ();

  exec.printStats ();
}

template <typename Iter, typename Cmp, typename NhFunc, typename OpFunc, typename StableTest>
void for_each_ordered_rob (Iter beg, Iter end, Cmp cmp, NhFunc nhFunc, OpFunc opFunc, StableTest stabilityTest, const char* loopname=0) {

  for_each_ordered_rob (beg, end, cmp, nhFunc, opFunc, loopname);
}


template <typename T, typename Cmp, typename Exec>
class ROBparamContext: public ROBcontext<T, Cmp, Exec> {

  using Base = ROBcontext<T, Cmp, Exec>;

public:
  const size_t step; 

  ROBparamContext (const T& x, Exec& e, const size_t _step): Base (x, e), step (_step) {}

  virtual void subAcquire (Lockable* l) {
    for (bool succ = false; !succ; ) {
      typename Base::AcquireStatus acq = Base::tryAcquire (l);

      switch (acq) {
        case Base::FAIL: {
          ROBparamContext* that = static_cast<ROBparamContext*> (Base::getOwner (l));
          assert (that != nullptr);
          bool abortSelf = resolveConflict (that, l);
          succ = abortSelf;
          break;
        }

        case Base::NEW_OWNER: {
          Base::nhood.push_back (l);
          succ = true;
          break;
        }

        case Base::ALREADY_OWNER: {
          assert (std::find (Base::nhood.begin (), Base::nhood.end (), l) != Base::nhood.end ());
          succ = true;
          break;
        }

        default: {
          GALOIS_DIE ("invalid acquire status");
          break;
        }
      }
    }
  }


private:

  bool resolveConflict (ROBparamContext* that, const Lockable* const l) {
    // this can be in SCHEDULED or ABORT_SELF
    // that can be in READY_TO_COMMIT only
    // return true if this aborts self

    assert (this->hasState (Base::SCHEDULED) || this->hasState (Base::ABORT_SELF));
    assert (that->hasState (Base::READY_TO_COMMIT));
    
    bool ret = false;
    if (Base::executor.getCtxtCmp () (this, that)) {
      assert (that->hasState (Base::READY_TO_COMMIT));
      that->doAbort ();
      dbg::debug (this, " aborting ", that, " on lock ", l);

    } else {
      ret = true;
    }

    return ret;
  }

};

template <typename T, typename Cmp, typename NhFunc, typename OpFunc>
class ROBparaMeter: private boost::noncopyable {

  using Ctxt = ROBparamContext<T, Cmp, ROBparaMeter>;
  using CtxtAlloc = Galois::Runtime::MM::FSBGaloisAllocator<Ctxt>;
  using CtxtCmp = typename Ctxt::PtrComparator;
  using CtxtDeq = Galois::Runtime::PerThreadDeque<Ctxt*>;

  using PendingQ = Galois::MinHeap<T, Cmp>;
  using ROB = Galois::MinHeap<Ctxt*, typename Ctxt::PtrComparator>;
  using ExecutionRecord = std::vector<size_t>;

  Cmp itemCmp;
  NhFunc nhFunc;
  OpFunc opFunc;
  CtxtCmp ctxtCmp;

  PendingQ* currPending;
  PendingQ* nextPending;
  ROB rob;
  CtxtAlloc ctxtAlloc;
  ExecutionRecord execRcrd;
  size_t steps;

public:
  ROBparaMeter (const Cmp& cmp, const NhFunc& nhFunc, const OpFunc& opFunc)
    : 
      itemCmp (cmp), 
      nhFunc (nhFunc), 
      opFunc (opFunc), 
      ctxtCmp (itemCmp),
      rob (ctxtCmp)
  {
    currPending = new PendingQ;
    nextPending = new PendingQ;
    steps = 0;
  }

  ~ROBparaMeter (void) {
    currPending->clear ();
    nextPending->clear ();
    delete currPending; currPending = nullptr;
    delete nextPending; nextPending = nullptr;
  }

  const Cmp& getItemCmp () const { return itemCmp; }

  const CtxtCmp& getCtxtCmp () const { return ctxtCmp; }

  template <typename Iter>
  void push (Iter beg, Iter end) {
    for (Iter i = beg; i != end; ++i) {
      nextPending->push (*i);
    }
  }

  void push (const T& x) {
    nextPending->push (x);
  }

  void execute () {

    while (!nextPending->empty () || !rob.empty ()) {

      ++steps;
      std::swap (currPending, nextPending);
      nextPending.clear ();
      execRcrd.push_back (0); // create record entry for current step;

      while (!currPending.empty ()) {
        Ctxt* ctx = schedule ();
        assert (ctx != nullptr);

        dbg::debug (ctx, " scheduled with item ", ctx->active);

        Galois::Runtime::setThreadContext (ctx);
        nhFunc (ctx->active, ctx->userHandle);

        if (ctx->hasState (Ctxt::SCHEDULED)) {
          opFunc (ctx->active, ctx->userHandle);
        }
        Galois::Runtime::setThreadContext (nullptr);

        if (ctx->hasState (Ctxt::SCHEDULED)) {
          ctx->setState (Ctxt::READY_TO_COMMIT);
          rob.push (ctx);

        } else {
          assert (ctx->hasState (Ctxt::ABORT_SELF));
          ctx->setState (Ctxt::ABORTING);
          ctx->doAbort ();
          nextPending->push (ctx->item);
        }
      }

      size_t numCommitted = clearROB ();

      assert (numCommitted > 0);

    }
  }
  
private:

  Ctxt* schedule () {

    assert (!currPending->empty ());

    Ctxt* ctx = ctxtAlloc.allocate (1);
    assert (ctx != nullptr);
    assert (steps > 0);
    ctxtAlloc.construct (ctx, currPending->pop (), *this, (steps-1));

    ctx->setState (Ctxt::SCHEDULED);

    return ctx;
  }

  size_t clearROB (void) {

    size_t numCommitted = 0;

    while (!rob.empty ()) {
      Ctxt* head = rob.top ();

      if (head->hasState (Ctxt::ABORT_DONE)) {
        rob.pop ();
        continue;

      } else if (head->hasState (Ctxt::READY_TO_COMMIT)) {
        assert (currPending->empty ());

        bool earliest = false;
        if (!nextPending->empty ()) {
          earliest = !itemCmp (nextPending->top (), head->active);

        } else {
          earliest = true;
        }

        if (earliest) {
          head->setState (COMMITTING);
          head->doCommit ();
          Ctxt* t = rob.pop ();
          assert (t == head);

          const size_t s = head->step;
          assert (s < execRcrd.size ());
          ++execRcrd[s];
          numCommitted += 1;


        } else {
          break;
        }

      } else {
        GALOIS_DIE ("head in rob with invalid status");
      }
    }

    return numCommitted;
  }



};




} // end namespace Runtime
} // end namespace Galois

#endif //  GALOIS_RUNTIME_ROB_EXECUTOR_H
