#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "kernel/list.h"

/* 세마포어 SEMA를 VALUE로 초기화합니다. 세마포어는
음이 아닌 정수와 그것을 조작하기 위한 두 가지 원자 연산자를
동반한 것입니다:

down 또는 "P": 값이 양수가 될 때까지 기다린 후,
그 값을 감소시킵니다.

up 또는 "V": 값을 증가시키고 (대기 중인 스레드가 있다면
하나를 깨웁니다). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/*-------------------------------------------------------------------------------------------------*/

/* 세마포어에 대한 다운 또는 "P" 연산. SEMA의 값이
양수가 될 때까지 기다린 다음 그 값을 원자적으로 감소시킵니다.

이 함수는 슬립(sleep)할 수 있으므로, 인터럽트 핸들러 내에서는
호출되어서는 안됩니다. 이 함수는 인터럽트가 비활성화된 상태에서
호출될 수 있지만, 슬립하면 다음에 스케줄된 스레드가 인터럽트를
다시 켜게 될 것입니다. 이것은 sema_down 함수입니다. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		// list_push_back (&sema->waiters, &thread_current ()->elem);
		list_insert_ordered(&sema->waiters, &thread_current ()->elem, sort_priority, 0);
		
		thread_block ();
	}
	sema->value--;

	intr_set_level (old_level);
}

/* 세마포어에 대한 다운 또는 "P" 연산이지만, 세마포어가 이미 0이 아닐 때만 수행합니다.
세마포어가 감소하면 true를 반환하고, 그렇지 않으면 false를 반환합니다.

이 함수는 인터럽트 핸들러에서 호출될 수 있습니다. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* 세마포어에 대한 업 또는 "V" 연산. SEMA의 값을 증가시키고,
SEMA를 기다리는 스레드 중 하나를 깨웁니다(있는 경우).
이 함수는 인터럽트 핸들러에서 호출될 수 있습니다. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();

	list_sort(&sema->waiters, sort_priority, 0);
	if (!list_empty (&sema->waiters))
		thread_unblock (list_entry (list_pop_front (&sema->waiters),
					struct thread, elem));
	sema->value++;

	change_list();

	intr_set_level (old_level);
}

/*-------------------------------------------------------------------------------------------------*/

static void sema_test_helper (void *sema_);

/* 스레드 쌍 사이에서 제어가 "핑퐁" 되도록 하는 세마포어에 대한 자체 테스트.
무슨 일이 벌어지는지 알아보려면 printf()를 호출하십시오. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* sema_self_test()에서 사용되는 스레드 함수. */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* LOCK을 초기화합니다. 잠금은 한 번에 하나의 스레드에 의해서만 보유될 수 있습니다.
우리의 잠금은 "재귀적"이 아닙니다, 즉, 현재 잠금을 보유하고 있는 스레드가 그 잠금을
획득하려고 시도하는 것은 오류입니다.

잠금은 초기 값이 1인 세마포어의 특수화입니다. 잠금과 이러한 세마포어 사이의 차이는
두 가지입니다. 첫째, 세마포어는 값이 1보다 큰 값을 가질 수 있지만, 잠금은 한 번에 하나의
스레드만이 소유할 수 있습니다. 둘째, 세마포어는 소유자가 없어서 한 스레드가 세마포어를
"다운"할 수 있고 그 다음에 다른 스레드가 "업"할 수 있지만, 잠금은 같은 스레드가
획득하고 릴리즈해야 합니다. 이러한 제한이 부담스러울 때는 세마포어를 사용하는 것이
좋은 신호입니다, 잠금 대신에. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/*-------------------------------------------------------------------------------------------------*/

#define MAX_DEPTH 8

void donate_priority(void) {
	int depth = 0;
	struct thread *t = thread_current();
	struct lock *l = t->wait_on_lock;

	while (l && depth < MAX_DEPTH) {
		depth++;
		if (!l->holder) break;
		
		// 락 소유자의 우선순위를 업데이트
		if (l->holder->priority < t->priority) {
			l->holder->priority = t->priority;
		}
		l = l->holder->wait_on_lock;
	}
}

/* 필요한 경우 잠자기까지 LOCK을 획득합니다.
이 잠금은 현재 스레드에 의해 이미 보유되어서는 안 됩니다.

이 함수는 잠들 수 있으므로 인터럽트 핸들러 내에서 호출되어서는 안 됩니다.
이 함수는 인터럽트가 비활성화된 상태에서 호출될 수 있지만,
잠들어야 하는 경우 인터럽트는 다시 켜질 것입니다. */
void lock_acquire (struct lock *lock) {
	
	struct thread * cur = thread_current();

	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	cur->wait_on_lock = lock;

	if(lock->holder != NULL){

		donate_priority();
	}

	sema_down (&lock->semaphore);
	lock->holder = cur;
	cur->wait_on_lock = NULL;
}

/* LOCK을 획득하려고 시도하고 성공한 경우 true를 반환하거나
실패한 경우 false를 반환합니다. 이 잠금은 현재 스레드에 의해 이미 보유되어서는 안 됩니다.

이 함수는 잠들지 않으므로 인터럽트 핸들러 내에서 호출될 수 있습니다. */
bool lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

void lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	struct list_elem *e;
	struct thread *cur = thread_current();

	for (e = list_begin (&cur->donations); e != list_end (&cur->donations); e = list_next (e)) {
		struct thread *f = list_entry (e, struct thread, donation_elem);
		if (f->wait_on_lock == lock) {
			list_remove(e);
		}
	}

	if(!list_empty(&cur->donations)) {
		list_sort(&cur->donations, sort_donate_priority, NULL);
		cur->priority = list_entry(list_front(&cur->donations), struct thread, donation_elem)->priority;
	} else {
		cur->priority = cur->initial_priority;
	}

	lock->holder = NULL;
	sema_up (&lock->semaphore);
}

/*-------------------------------------------------------------------------------------------------*/

/* 현재 스레드가 LOCK을 보유하고 있는 경우 true를 반환하고,
그렇지 않은 경우 false를 반환합니다.
(다른 스레드가 잠금을 보유하고 있는지 테스트하는 것은 위험할 수 있음에 주의하십시오.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* 리스트에 있는 하나의 세마포어. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

/* 조건 변수 COND를 초기화합니다. 조건 변수는
하나의 코드 조각이 조건을 신호로 보내고 협력하는 코드가
신호를 받아 그에 따라 작동하도록 합니다. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	// list_push_back (&cond->waiters, &waiter.elem);
	list_insert_ordered (&cond->waiters, &waiter.elem, sort_donate_priority, 0);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters)){

		list_sort(&cond->waiters, sort_donate_priority, 0);
		sema_up (&list_entry (list_pop_front (&cond->waiters), struct semaphore_elem, elem)->semaphore);
		
	}

}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
