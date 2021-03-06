#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

// pgdir lock for assign LWP
struct spinlock pgdirlock;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// stride
int mlfq_pass = 0;							// pass of mlfq
int mlfq_stride = (int)(10000 / 100);		// stride of mlfq, intial CPU share value is 100
int mlfq_share = 100;						// CPU share of mlfq

// mlfq
int totalticks = 0;							// for boosting
int q_count[3] = {-1, -1, -1};				// the number of process in queue per level
struct proc *q[3][NPROC];					// multi level queue
int allotment[3] = {5, 10, 1000};			// allotment per queue

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  // initailze for mlfq and stide scheduling
  // At first, all processes enter to queue of level 0
  p->level = 0;
  p->ticks = 0;
  p->cpu_share = 0;
  p->stride = 0;
  p->pass = 0;
  q_count[0]++;
  q[0][q_count[0]] = p;


  // Set LWP options
  p->is_LWP = 0;	// is process
  p->num_LWP = 0;
  p->all_LWP = 0;
  p->tid = -1;
  p->wtid = -1;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  if (curproc->is_LWP) {
	  sz = curproc->parent->sz;
	  if(n > 0){
		  if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
			  return -1;
	  } else if(n < 0){
		  if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
			  return -1;
	  }
	  curproc->parent->sz = sz;
  } else {
	  sz = curproc->sz;
	  if(n > 0){
		  if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
			  return -1;
	  } else if(n < 0){
		  if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
			  return -1;
	  }
	  curproc->sz = sz;
  }

  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);
  //cprintf("fork : %d\n", pid);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;
  int i, j, level;

  if(curproc == initproc)
    panic("init exiting");

  // if curproc is process and has no thread
  if (!curproc->is_LWP && !curproc->num_LWP) {
	  for(fd = 0; fd < NOFILE; fd++){
		  if(curproc->ofile[fd]){
			  fileclose(curproc->ofile[fd]);
			  curproc->ofile[fd] = 0;
		  }
	  }

	  begin_op();
	  iput(curproc->cwd);
	  end_op();
	  curproc->cwd = 0;

	  acquire(&ptable.lock);

	  // Parent might be sleeping in wait().
	  wakeup1(curproc->parent);

	  // Pass abandoned children to init.
	  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		  if(p->parent == curproc){
			  p->parent = initproc;
			  if(p->state == ZOMBIE)
				  wakeup1(initproc);
		  }
	  }

	  // Jump into the scheduler, never to return.
	  curproc->state = ZOMBIE;
	  if (curproc->stride == 0) {
		  level = curproc->level;
		for (i = 0; i <= q_count[level]; i++)
			if (curproc == q[level][i]) {
				for (j = i; j <= q_count[level] - 1; j++)
					q[level][j] = q[level][j + 1];
				q[level][q_count[level]] = 0;
				q_count[level]--;
				break;
			}
	} 
	else {
		// if p is in stride
		curproc->pass = 0;
	}
	  sched();
	  panic("zombie exit");

  }

	// if curproc is process and has some threads
	else if (!curproc->is_LWP && curproc->num_LWP) {
	acquire(&ptable.lock);
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if (p->parent == curproc && p->is_LWP){
			release(&ptable.lock);

		//if (p->parent != curproc || !p->is_LWP)
		//	continue;
		for(fd = 0; fd < NOFILE; fd++){
			if(p->ofile[fd]){
			  fileclose(p->ofile[fd]);
			  p->ofile[fd] = 0;
		  }
		}

		begin_op();
		iput(p->cwd);
		end_op();
		p->cwd = 0;

		acquire(&ptable.lock);
		p->parent->num_LWP--;

		/*
		if (!p->parent->num_LWP && p->parent->all_LWP) {
			p->parent->sz = deallocuvm(p->parent->pgdir, p->parent->sz, p->parent->sz - (p->parent->all_LWP) * 2 * PGSIZE);
			p->parent->all_LWP = 0;
		}*/
        kfree(p->kstack);
        p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
		p->state = UNUSED;
		if (p->stride == 0) {
			level = p->level;
			for (i = 0; i <= q_count[level]; i++)
				if (p == q[level][i]) {
					for (j = i; j <= q_count[level] - 1; j++)
						q[level][j] = q[level][j + 1];
					q[level][q_count[level]] = 0;
					q_count[level]--;
					break;
				}
		} 
		else {
			// if p is in stride
			p->pass = 0;
		}

		// initailize variables for sceduling
		p->level = 0;
		p->ticks = 0;
		mlfq_share += p->cpu_share;
		mlfq_stride = (int) (10000 / mlfq_share);
		p->cpu_share = 0;
		p->stride = 0;
		p->pass = 0;
		// initialize variables for LWP
		p->is_LWP = 0;
		p->num_LWP = 0;
		p->all_LWP = 0;
		p->tid = -1;
		p->wtid = -1;
	}}
	release(&ptable.lock);

	for(fd = 0; fd < NOFILE; fd++){
		if(curproc->ofile[fd]){
			fileclose(curproc->ofile[fd]);
			curproc->ofile[fd] = 0;
		}
	}

	begin_op();
	iput(curproc->cwd);
	end_op();
	curproc->cwd = 0;

	acquire(&ptable.lock);
	// Parent might be sleeping in wait().
	wakeup1(curproc->parent);


	// Jump into the scheduler, never to return.
	curproc->state = ZOMBIE;
	if (curproc->stride == 0) {
		level = curproc->level;
		for (i = 0; i <= q_count[level]; i++)
			if (curproc == q[level][i]) {
				for (j = i; j <= q_count[level] - 1; j++)
					q[level][j] = q[level][j + 1];
				q[level][q_count[level]] = 0;
				q_count[level]--;
				break;
			}
	} 
	else {
		// if p is in stride
		curproc->pass = 0;
	}

	sched();
	panic("zombie exit");
  }
   else if (curproc->is_LWP) {
	   struct proc * parent = curproc->parent;
	   acquire(&ptable.lock);

	   for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if (p->parent == curproc->parent && p->is_LWP && p != curproc){
			release(&ptable.lock);
		for(fd = 0; fd < NOFILE; fd++){
			if(p->ofile[fd]){
			  fileclose(p->ofile[fd]);
			  p->ofile[fd] = 0;
		  }
		}

		begin_op();
		iput(p->cwd);
		end_op();
		p->cwd = 0;
		
		acquire(&ptable.lock);
		p->parent->num_LWP--;

		kfree(p->kstack);
		p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
		p->killed = 0;
		p->state = UNUSED;
		if (p->stride == 0) {
			level = p->level;
			for (i = 0; i <= q_count[level]; i++)
				if (p == q[level][i]) {
					for (j = i; j <= q_count[level] - 1; j++)
						q[level][j] = q[level][j + 1];
					q[level][q_count[level]] = 0;
					q_count[level]--;
					break;
				}
		} 
		else {
			// if p is in stride
			p->pass = 0;
		}

		// initailize variables for sceduling
		p->level = 0;
		p->ticks = 0;
		mlfq_share += p->cpu_share;
		mlfq_stride = (int) (10000 / mlfq_share);
		p->cpu_share = 0;
		p->stride = 0;
		p->pass = 0;
		// initialize variables for LWP
		p->is_LWP = 0;
		p->num_LWP = 0;
		p->all_LWP = 0;
		p->tid = -1;
		p->wtid = -1;
	   }}
	   release(&ptable.lock);

	   for(fd = 0; fd < NOFILE; fd++){
		   if(curproc->ofile[fd]){
			   fileclose(curproc->ofile[fd]);
			   curproc->ofile[fd] = 0;
		   }
	   }

	   begin_op();
	   iput(curproc->cwd);
	   end_op();
	   curproc->cwd = 0;

	   curproc->parent->num_LWP--;

	   if (!parent->num_LWP && parent->all_LWP) {
		   parent->sz = deallocuvm(parent->pgdir, parent->sz, parent->sz - (parent->all_LWP - 1) * 2 * PGSIZE);
		   parent->all_LWP = 0;
	   }
	   curproc->parent = curproc;
	   curproc->state = ZOMBIE;
	   if (curproc->stride == 0) {
		   level = curproc->level;
		   for (i = 0; i <= q_count[level]; i++)
			   if (curproc == q[level][i]) {
				   for (j = i; j <= q_count[level] - 1; j++)
					   q[level][j] = q[level][j + 1];
				   q[level][q_count[level]] = 0;
				   q_count[level]--;
				   break;
			   }
	   } 
	   else {
		   // if p is in stride
		   curproc->pass = 0;
	   }

		for(fd = 0; fd < NOFILE; fd++){
		   if(parent->ofile[fd]){
			   fileclose(parent->ofile[fd]);
			   parent->ofile[fd] = 0;
		   }
	   }

	   begin_op();
	   iput(parent->cwd);
	   end_op();
	   parent->cwd = 0;
		
	   acquire(&ptable.lock);
	   // Parent might be sleeping in wait().
	   wakeup1(parent->parent);

	   // Jump into the scheduler, never to return.
	   parent->state = ZOMBIE;
		if (parent->stride == 0) {
			level = parent->level;
			for (i = 0; i <= q_count[level]; i++)
				if (parent == q[level][i]) {
					for (j = i; j <= q_count[level] - 1; j++)
						q[level][j] = q[level][j + 1];
					q[level][q_count[level]] = 0;
					q_count[level]--;
					break;
				}
		} 
		else {
			// if p is in stride
			parent->pass = 0;
		}

	   sched();
	   panic("zombie exit");

   }
}
	

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  int level, i, j;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
		// if p is in mlfq, must dequeue!!
		level = p->level;
		if (p->stride == 0) {
			for (i = 0; i <= q_count[level]; i++) {
				if (p == q[level][i]) {
					for (j = i; j <= q_count[level] - 1; j++)
						q[level][j] = q[level][j + 1];
					q_count[level]--;
					break;
				}
			}	
		}
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
		// initailize variables for sceduling
		p->level = 0;
		p->ticks = 0;
		mlfq_share += p->cpu_share;
		mlfq_stride = (int) (10000 / mlfq_share);
		p->cpu_share = 0;
		p->stride = 0;
		p->pass = 0;
		// initialize variables for LWP
		p->is_LWP = 0;
		p->num_LWP = 0;
		p->all_LWP = 0;
		p->tid = -1;
		p->wtid = -1;
	
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

int
set_cpu_share(int share) {

	struct proc *p, *ip;
	int min_pass = mlfq_pass;
	int i, j;
	
	// no negative share
	if (share < 0) {
		return -1;
	}

	// Total stride processes are able to get at most 80% of CPU time
	if (mlfq_share - share <= 20) {
		return -1;
	}

	acquire(&ptable.lock);

	// Find minimum pass of process 
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if (p->state == RUNNABLE && p->stride != 0)
			min_pass = (min_pass > p->pass) ? p->pass : min_pass;
	}

	p = myproc();

	// delete in mlfq
	for (i = 0; i <= q_count[p->level]; i++) {
		if (p == q[p->level][i]) {
			for (j = i; j < q_count[p->level]; j++)
				q[p->level][j] = q[p->level][j + 1];
		}
	}
	q[p->level][q_count[p->level]] = 0;
	q_count[p->level]--;

	// initialize variables for stride scheduling
	mlfq_share -= share;
	mlfq_stride = (int)(10000 / mlfq_share);
	if(p->num_LWP > 0) {
		int avg_share = (int)(share / p->num_LWP + 1);
		p->cpu_share = avg_share;
		p->stride = (int) (10000 / avg_share);
		p->pass = min_pass;
		for (ip = ptable.proc; ip < &ptable.proc[NPROC]; ip++) {
			if (ip->parent == p) {
				ip->cpu_share = avg_share;
				ip->stride = (int) (10000 / avg_share); 
				ip->pass = min_pass;
			}
		}
	} else {
		p->cpu_share = share;
		p->stride = (int)(10000 / share);
		p->pass = min_pass;
	}

	release(&ptable.lock);
	return share;
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  int i, j;
  int level;
  int mlfq_turn = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    acquire(&ptable.lock);
	struct proc *min = 0;
	int min_pass = mlfq_pass;

	// Find minimum pass of process 
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
	  if (p->state == RUNNABLE && p->cpu_share != 0 && p->pass <= min_pass) {
		min = p;
		min_pass = p->pass;
	  }
	}

	// if not mlfq is minimum pass
	if (min) {
		p = min;
		p->pass += p->stride;
		c->proc = p;
		switchuvm(p);
		p->state = RUNNING;
		swtch(&c->scheduler, p->context);
		switchkvm();
		c->proc = 0;
	} 
	// if mlfq is minimum pass
	else {
		mlfq_pass += mlfq_stride;
		mlfq_turn = 1;

		// boosting!!!!
		if (totalticks >= 100) {
			//cprintf("[do boosting!]\n");
			for (i = 0, j = q_count[0] + 1; i <= q_count[1]; i++, j++) {
				p = q[1][i];
				q[0][j] = p;
				p->level = 0;
				p->ticks = 0;
				q[1][i] = 0;
				q_count[0]++;
			}
			q_count[1] = -1;
			for (i = 0, j = q_count[0] + 1; i <= q_count[2]; i++, j++) {
				p = q[2][i];
				q[0][j] = p;
				p->level = 0;
				p->ticks = 0;
				q[2][i] = 0;
				q_count[0]++;
			}
			q_count[2] = -1;
			totalticks = 0;
		}

		for (level = 0; level < 3; level++) {
			// if upper level queue has process
			if (mlfq_turn) {
				if ((level == 1 && q_count[0] != -1) ||
						(level == 2 && q_count[1] != -1 
						 && q_count[2] != -1))  {
					level = -1;
					continue;
				}	
			}
			if (q_count[level]!= -1 && mlfq_turn) {
				for (i = 0; i <= q_count[level]; i++) {
					if (q[level][i]->state != RUNNABLE) 
						continue;
					//cprintf("[%d] cnt_queue : %d, tid = %d\n", level, q_count[level], q[level][i]->tid);
					p = q[level][i];
					c->proc = q[level][i];
					switchuvm(p);
					p->state = RUNNING;
					swtch(&c->scheduler, p->context);
					switchkvm();
					mlfq_turn = 0;

					// If a process uses too much CPU time, it will be moved to a lower-priority queue.
					if (level != 2 && p->ticks >= allotment[level]) {
						q_count[level + 1]++;
						c->proc->level++;
						c->proc->ticks = 0;
						q[level + 1][q_count[level + 1]] = c->proc;

						// delete process in queue
						for (j = i; j <= q_count[level] - 1; j++)
							q[level][j] = q[level][j + 1];

						q[level][q_count[level]] = 0;
						q_count[level]--;
					}
					c->proc = 0;
				}
			}
		}
	}
	release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  int level;
  int i, j;
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // if p is in mlfq, must delete in mlfq
  if (p->stride == 0) {
	  level = p->level;
	  for (i = 0; i <= q_count[level]; i++)
		  if (p == q[level][i]) {
			  for (j = i; j <= q_count[level] - 1; j++)
				  q[level][j] = q[level][j + 1];
			  q[level][q_count[level]] = 0;
			  q_count[level]--;
			  break;
		  }
  } 
  else {
  // if p is in stride
	  p->pass = 0;
  }

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p, *sp;
  int i;
  int min_pass = mlfq_pass;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan) {
		p->ticks = 0;
		p->level = 0;
		p->state = RUNNABLE;
		// if process is in mlfq
		if (p->stride == 0) {
			q_count[0]++;
			for (i = q_count[0]; i > 0; i--)
				q[0][i] = q[0][i - 1];
			q[0][0] = p;
		} // if process is in stride
		else {
			for (sp = ptable.proc; sp < &ptable.proc[NPROC]; sp++)
				if (sp != p && sp->stride && sp->state == RUNNABLE)
					min_pass = (min_pass > sp->pass) ? sp->pass : min_pass;
			p->pass = min_pass;
		}
	}
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
// Create threads within the process. 
// From that point on, 
// the execution routine assigned to each thread starts.
// Return thread id
int
thread_create(thread_t * thread, void * (*start_routine)(void *), void *arg) 
{
	struct proc *np, *p;
	struct proc *curproc = myproc();
	uint sp, ustack[2];
	int i, avg_share;

	// Allocate thread
	if((np = allocproc()) == 0) {
		return -1;
	}

	acquire(&pgdirlock);
	// assgin new stack memory for LWP
	curproc->sz = PGROUNDUP(curproc->sz);
	if ((curproc->sz = allocuvm(curproc->pgdir, curproc->sz, curproc->sz + 2 * PGSIZE)) == 0) {
		return -1;
	}

	sp = curproc->sz;

	// Set thread options
	np->is_LWP = 1;
	np->parent = curproc;
	np->tid = curproc->num_LWP++;
	np->all_LWP++;
	np->pgdir = curproc->pgdir;
	np->sz = curproc->sz;
	*np->tf = *curproc->tf;

	// set return value
	*thread = np->tid;
	
	release(&pgdirlock);

	ustack[0] = 0xffffffff;
	ustack[1] = (uint)arg;

	sp -= 8;

	if (copyout(np->pgdir, sp, ustack, 8) < 0)
		return -1;

	np->tf->eax = 0;
	np->tf->eip = (uint)start_routine;
	np->tf->esp = sp;

	for(i = 0; i < NOFILE; i++)
		if(curproc->ofile[i])
			np->ofile[i] = filedup(curproc->ofile[i]);
	np->cwd = idup(curproc->cwd);

	safestrcpy(np->name, curproc->name, sizeof(curproc->name));

	switchuvm(curproc);

	acquire(&ptable.lock);

	// If main thread is in stride scheduling, 
	// assign new stride to all threads
	
	np->state = RUNNABLE;

	if(np->parent->cpu_share != 0) {
		avg_share = (int)(np->parent->cpu_share / np->parent->num_LWP + 1);
		np->parent->cpu_share = avg_share;
		np->parent->stride = (int) (10000 / avg_share);
		for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
			if (p->parent == np->parent) {
				p->cpu_share = np->parent->cpu_share;
				p->stride = (int) (10000 / p->cpu_share); 
				p->pass = np->parent->pass;
			}
		}
	}

	release(&ptable.lock);

	return 0;
}

// You must provide a method to terminate the thread in it. 
// As the main function do, 
// you call the thread_exit function at the last of a thread routine. 
// Through this function, you must able to return a result of a thread.
void
thread_exit(void * retval) 
{
	int fd;
	struct proc *curproc = myproc();
	//int i;
	//int min_pass = mlfq_pass;
	struct proc *p;
	//struct proc  * sp;
	int i, j, level;

	if (curproc == initproc)
		panic("init existing");

	for(fd = 0; fd < NOFILE; fd++){
		if(curproc->ofile[fd]){
			fileclose(curproc->ofile[fd]);
			curproc->ofile[fd] = 0;
		}
	}

	begin_op();
	iput(curproc->cwd);
	end_op();
	curproc->cwd = 0;

	//cprintf("try ptable lock~\n");
	acquire(&ptable.lock);
	//cprintf("lock ptablelock~~\n");
	wakeup1(curproc->parent);

	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if (p->parent == curproc) {
			p->parent = initproc;
			if (p->state == ZOMBIE)
				wakeup1(initproc);
		}
	}

	curproc->state = ZOMBIE;
	//cprintf("zombie tid : %d\n", curproc->tid);
	curproc->retval = retval;
	// if p is in mlfq, must delete in mlfq
	if (curproc->stride == 0) {
		level = curproc->level;
		for (i = 0; i <= q_count[level]; i++)
			if (curproc == q[level][i]) {
				for (j = i; j <= q_count[level] - 1; j++)
					q[level][j] = q[level][j + 1];
				q[level][q_count[level]] = 0;
				q_count[level]--;
				break;
			}
	} 
	else {
		// if p is in stride
		curproc->pass = 0;
	}

	sched();
	panic("zombie exit");
}

// You must provide a method to wait for the thread 
// specified by the argument to terminate. 
// If that thread has already terminated, 
// then this returns immediately. 
// In the join function, you have to clean up the resources 
// allocated to the thread such as a page table, allocated memories and stacks. 
// You can get the return value of thread through this function.
int
thread_join(thread_t thread, void **retval)
{
	struct proc *p;
	int level, i, j, havekids;
	struct proc *curproc = myproc();
	curproc->wtid = thread;

	acquire(&ptable.lock);
	for(;;){
		havekids = 0;
		// Scan through table looking for exited children.
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
			if(p->parent != curproc)
				continue;
			//cprintf("try find tid : %d, %d\n", thread, p->tid);
			havekids = 1;
			if(p->state == ZOMBIE && p->tid == thread){
				//cprintf("find %d, tid : %d\n", thread, p->tid);
				//cprintf("tid : %d\n",p->tid);
				// if p is in mlfq, must dequeue!!
				level = p->level;
				if (p->stride == 0) {
					for (i = 0; i <= q_count[level]; i++) {
						if (p == q[level][i]) {
							for (j = i; j <= q_count[level] - 1; j++)
								q[level][j] = q[level][j + 1];
							q_count[level]--;
							break;
						}
					}	
				}

				*retval = p->retval;

				// Found one.
				kfree(p->kstack);
				p->kstack = 0;
				p->pid = 0;
				p->parent = 0;
				p->name[0] = 0;
				p->killed = 0;
				// initailize variables for sceduling
				p->level = 0;
				p->ticks = 0;
				mlfq_share += p->cpu_share;
				mlfq_stride = (int) (10000 / mlfq_share);
				p->cpu_share = 0;
				p->stride = 0;
				p->pass = 0;
				// initialize variables for LWP
				p->is_LWP = 0;
				p->num_LWP = 0;
				p->all_LWP = 0;
				p->tid = -1;
				p->wtid = -1;

				p->state = UNUSED;
				release(&ptable.lock);
				return 0;
			}
		}
			
		// No point waiting if we don't have any children.
		if(curproc->killed || !havekids){
			release(&ptable.lock);
			return -1;
		}

		// Wait for children to exit.  (See wakeup1 call in proc_exit.)
		sleep(curproc, &ptable.lock);  //DOC: wait-sleep
	}
	return 0;
}


