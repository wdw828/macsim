/**********************************************************************************************
 * File         : schedule_io.cc
 * Author       : Hyesoon Kim
 * Date         : 1/1/2008 
 * SVN          : $Id: main.cc,v 1.26 2008-09-21 00:02:54 kacear Exp $:
 * Description  : scheduler for gpu (small many cores)
 *********************************************************************************************/


#include "allocate_smc.h"
#include "schedule_smc.h"
#include "pqueue.h"
#include "exec.h"
#include "core.h"
#include "memory.h"
#include "rob_smc.h"

#include "knob.h"
#include "debug_macros.h"
#include "statistics.h"

#include "all_knobs.h"

#define DEBUG(args...)   _DEBUG(*m_simBase->m_knobs->KNOB_DEBUG_SCHEDULE_STAGE, ## args) 


///////////////////////////////////////////////////////////////////////////////////////////////
/// \page page1 GPU instruction scheduler
/// How this scheduler is implemented: \n
/// \section asdf
/// The instruction window will hold instructions from multiple threads. the 
/// scheduler can switch from one thread to another in any order (depending 
/// on the policy), but instructions within a thread must be scheduled inorder.
/// for these reasons, the scheduler is implemented as a list of scheduler 
/// queues. each thread is assigned a queue when the thread (block) is assigned 
/// to the core. when the thread terminates, the queue is freed.
///////////////////////////////////////////////////////////////////////////////////////////////


// schedule_smc_c constructor
schedule_smc_c::schedule_smc_c(int core_id, pqueue_c<gpu_allocq_entry_s>** gpu_allocq,
    smc_rob_c* gpu_rob, exec_c* exec, Unit_Type unit_type, frontend_c* frontend, 
    macsim_c* simBase) :  m_gpu_rob(gpu_rob), m_gpu_allocq(gpu_allocq), 
    schedule_c(exec, core_id, unit_type, frontend, NULL, simBase)
{

  m_simBase = simBase;

  // configuration
  switch (m_unit_type) {
    case UNIT_SMALL:
      knob_num_threads      = *m_simBase->m_knobs->KNOB_MAX_THREADS_PER_CORE;
      break;
    case UNIT_MEDIUM:
      knob_num_threads      = *m_simBase->m_knobs->KNOB_MAX_THREADS_PER_MEDIUM_CORE;
      break;
    case UNIT_LARGE:
      knob_num_threads      = *m_simBase->m_knobs->KNOB_MAX_THREADS_PER_LARGE_CORE;
      break;
  }

#if 0
  m_schedule_lists     = new int *[knob_num_threads];
  m_first_schlist_ptrs = new int[knob_num_threads];
  m_last_schlist_ptrs  = new int[knob_num_threads];
  m_schedule_arbiter   = new int[*m_simBase->m_knobs->KNOB_NUM_WARP_SCHEDULER];
  fill_n(m_schedule_arbiter, static_cast<int>(*m_simBase->m_knobs->KNOB_NUM_WARP_SCHEDULER), -1);
#endif
  m_schedule_modulo = (*m_simBase->m_knobs->KNOB_GPU_SCHEDULE_RATIO - 1); 
  m_schlist_size = MAX_GPU_SCHED_SIZE, knob_num_threads;
  m_schlist_entry   = new int[m_schlist_size];
  m_schlist_tid     = new int[m_schlist_size];
  m_first_schlist   = 0;
  m_last_schlist    = 0;

#if 0
  for (int i = 0; i < knob_num_threads; ++i) {
    m_schedule_lists[i]     = new int[MAX_GPU_SCHED_SIZE];
    ASSERT(m_schedule_lists[i]);

    fill_n(m_schedule_lists[i], MAX_GPU_SCHED_SIZE, -1);
    m_first_schlist_ptrs[i] = 0;
    m_last_schlist_ptrs[i]  = 0;
    m_free_list.push_back(i);
  }
#endif
}


// schedule_smc_c destructor
schedule_smc_c::~schedule_smc_c(void) {
#if 0
  for (int i = 0; i < knob_num_threads; i++) {
    delete [] m_schedule_lists[i];
  }
  delete [] m_schedule_lists;
  delete [] m_first_schlist_ptrs;
  delete [] m_last_schlist_ptrs;
#endif
  delete[] m_schlist_entry;
  delete[] m_schlist_tid;
}


// move uops from alloc queue to schedule queue
void schedule_smc_c::advance(int q_index) {
  fill_n(m_count, static_cast<size_t>(max_ALLOCQ), 0);

  while (m_gpu_allocq[q_index]->ready()) {
    gpu_allocq_entry_s  allocq_entry = m_gpu_allocq[q_index]->peek(0); 

    int tid        = allocq_entry.m_thread_id; 
    rob_c *m_rob   = m_gpu_rob->get_thread_rob(tid);
    uop_c *cur_uop = (uop_c *) (*m_rob)[allocq_entry.m_rob_entry];
    
    STAT_CORE_EVENT(m_core_id, POWER_INST_QUEUE_R);
    STAT_CORE_EVENT(m_core_id, POWER_REORDER_BUF_R);
    STAT_CORE_EVENT(m_core_id, POWER_UOP_QUEUE_R);
    STAT_CORE_EVENT(m_core_id, POWER_REG_RENAMING_TABLE_R);
    STAT_CORE_EVENT(m_core_id, POWER_FREELIST_R);

    ALLOCQ_Type allocq = (*m_rob)[allocq_entry.m_rob_entry]->m_allocq_num;
    if ((m_count[allocq] >= m_sched_rate[allocq]) ||
    	(m_num_per_sched[allocq] >= m_sched_size[allocq])) {
      break;
    }


    // dequeue the element from the alloc queue
    m_gpu_allocq[q_index]->dequeue();
    

    // if the entry has been flushed
    if (cur_uop->m_bogus || (cur_uop->m_done_cycle) ) {
      cur_uop->m_done_cycle = (m_simBase->m_core_cycle[m_core_id]);
      continue;
    }


    // update the element m_count for corresponding scheduled queue
    m_count[allocq]         = m_count[allocq]+1;
    cur_uop->m_in_iaq       = false;
    cur_uop->m_in_scheduler = true;

//    int queue = get_reserved_sched_queue(allocq_entry.m_thread_id);
    int entry = allocq_entry.m_rob_entry;

    m_schlist_entry[m_last_schlist] = entry;
    m_schlist_tid[m_last_schlist++] = tid;
    m_last_schlist %= m_schlist_size;
    ++m_num_in_sched;
    ++m_num_per_sched[allocq];
   	
    STAT_CORE_EVENT(m_core_id, POWER_RESERVATION_STATION_W);

#if 0
    // update counters 
    m_schedule_lists[queue][m_last_schlist_ptrs[queue]] = entry;
    m_last_schlist_ptrs[queue]++;
    m_last_schlist_ptrs[queue] %= MAX_GPU_SCHED_SIZE;
    m_num_in_sched++;
    m_num_per_sched[allocq]++;

    DEBUG("cycle_m_count:%lld m_num_in_sched:%d entry:%d (*m_rob)"
        "[allocq_entry.rob_entry]->in_scheduler:%d allocq_number:%d q_index:%d \n",
        m_cur_core_cycle, m_num_in_sched, allocq_entry.m_rob_entry,
        (*m_rob)[allocq_entry.m_rob_entry]->m_in_scheduler, allocq, q_index);
#endif
  }
}


// check source registers are ready
bool schedule_smc_c::check_srcs(int thread_id, int entry)
{
  bool ready = true;
  uop_c *cur_uop = NULL;
  rob_c *thread_m_rob = m_gpu_rob->get_thread_rob(thread_id);
  cur_uop = (*thread_m_rob)[entry];
  

  // check if all sources are already ready
  if (cur_uop->m_srcs_rdy) {
    return true;  
  }

  for (int i = 0; i < cur_uop->m_num_srcs; ++i) {
    if (cur_uop->m_map_src_info[i].m_uop == NULL) {
      continue;
    }


    // Extract the source uop info
    uop_c* src_uop = cur_uop->m_map_src_info[i].m_uop;
    uns src_uop_num = cur_uop->m_map_src_info[i].m_uop_num;
    

    // Check if source uop is valid
    if (!src_uop || 
        !src_uop->m_valid || 
        (src_uop->m_uop_num != src_uop_num) ||
        (src_uop->m_thread_id != cur_uop->m_thread_id))  {
      continue;
    }

    DEBUG("core_cycle_m_count:%lld core_id:%d thread_id:%d uop_num:%lld "
        "src_uop_num:%u src_uop->uop_num:%lld src_uop->done_cycle:%lld "
        "src_uop->uop_num:%s  src_uop_num:%s \n", m_cur_core_cycle, m_core_id,
        cur_uop->m_thread_id, cur_uop->m_uop_num, src_uop_num, src_uop->m_uop_num, 
        src_uop->m_done_cycle, unsstr64(src_uop->m_uop_num), unsstr64(src_uop_num));

    // Check if the source uop is ready
    if ((src_uop->m_done_cycle  == 0) || 
        (m_simBase->m_core_cycle[m_core_id] < src_uop->m_done_cycle))  {
      // Source is not ready. 
      // Hence we update the last_dep_exec field of this uop and return. 
      if (!cur_uop->m_last_dep_exec || 
          (*(cur_uop->m_last_dep_exec) < src_uop->m_done_cycle)) {
        DEBUG("*cur_uop->last_dep_exec:%lld src_uop->uop_num:%lld src_uop->done_cycle:%lld \n",
              cur_uop->m_last_dep_exec ? *(cur_uop->m_last_dep_exec): 0, 
              src_uop?src_uop->m_uop_num: 0, src_uop? src_uop->m_done_cycle: 1);

        cur_uop->m_last_dep_exec = &(src_uop->m_done_cycle);
      }
      ready = false;
      return ready;
    }
  }


  //The uop is ready since we didnt find any source uop that was not ready
  cur_uop->m_srcs_rdy = ready;

  return ready;
}


// schedule an uop from reorder buffer
// called by schedule_io_c::run_a_cycle
// call exec_c::exec function for uop execution
bool schedule_smc_c::uop_schedule(int thread_id, int entry, SCHED_FAIL_TYPE* sched_fail_reason)
{
  uop_c *cur_uop = NULL;
  rob_c *thread_m_rob = m_gpu_rob->get_thread_rob(thread_id);

  cur_uop     = (*thread_m_rob)[entry];
  int q_num  = cur_uop->m_allocq_num;
  bool bogus = cur_uop->m_bogus;

  *sched_fail_reason = SCHED_SUCCESS;
  
  STAT_CORE_EVENT(m_core_id, POWER_RESERVATION_STATION_R_TAG);
  STAT_CORE_EVENT(m_core_id, POWER_INST_ISSUE_SEL_LOGIC_R);
  STAT_CORE_EVENT(m_core_id, POWER_PAYLOAD_RAM_R);
    
  DEBUG("uop_schedule core_id:%d thread_id:%d uop_num:%lld inst_num:%lld "
      "uop.va:%s allocq:%d mem_type:%d last_dep_exec:%llu done_cycle:%llu\n",
      m_core_id, cur_uop->m_thread_id, cur_uop->m_uop_num, cur_uop->m_inst_num, 
      hexstr64s(cur_uop->m_vaddr), cur_uop->m_allocq_num, cur_uop->m_mem_type, 
      (cur_uop->m_last_dep_exec? *(cur_uop->m_last_dep_exec) : 0), cur_uop->m_done_cycle); 


  // Return if sources are not ready 
  if (!bogus && !(cur_uop->m_srcs_rdy) && cur_uop->m_last_dep_exec &&
		  (m_cur_core_cycle < *(cur_uop->m_last_dep_exec))) {
    *sched_fail_reason = SCHED_FAIL_OPERANDS_NOT_READY;
    return false;
  }


  if (!bogus) {
    // source registers are not ready
    if (!check_srcs(thread_id, entry)) {
      *sched_fail_reason = SCHED_FAIL_OPERANDS_NOT_READY;
      DEBUG("core_id:%d thread_id:%d uop_num:%lld operands are not ready \n", 
            m_core_id, cur_uop->m_thread_id, cur_uop->m_uop_num); 

      return false;
    }   
  

    // Check for port availability.
    if (!m_exec->port_available(q_num)) {
      *sched_fail_reason = SCHED_FAIL_NO_AVAILABLE_PORTS;
      DEBUG("core_id:%d thread_id:%d uop_num:%lld ports are not ready \n", 
          m_core_id, cur_uop->m_thread_id, cur_uop->m_uop_num); 
      return false;
    }


    // check available mshr spaces for scheduling
    core_c *core = m_simBase->m_core_pointers[m_core_id];
    if ("ptx" == core->get_core_type() && 
        cur_uop->m_mem_type != NOT_MEM && 
        cur_uop->m_num_child_uops > 0) {
      // constant or texture memory access
      if (cur_uop->m_mem_type == MEM_LD_CM || cur_uop->m_mem_type == MEM_LD_TM) {
        if (!m_simBase->m_memory->get_num_avail_entry(m_core_id)) {
          *sched_fail_reason = SCHED_FAIL_NO_MEM_REQ_SLOTS;
          return false;
        }
      }
    }
  }

  cur_uop->m_state = OS_SCHEDULE;
  STAT_CORE_EVENT(m_core_id, POWER_RESERVATION_STATION_R);


  // -------------------------------------
  // execute current uop
  // -------------------------------------
  if (!m_exec->exec(thread_id, entry, cur_uop)) {
    // uop could not execute
    DEBUG("core_id:%d thread_id:%d uop_num:%lld just cannot be executed\n", 
          m_core_id, cur_uop->m_thread_id, cur_uop->m_uop_num); 

    return false;
  }


  // Generate Stat events
  STAT_EVENT(DISPATCHED_INST);
  STAT_EVENT_N(DISPATCH_WAIT, m_cur_core_cycle - cur_uop->m_alloc_cycle);
  STAT_CORE_EVENT(m_core_id, CORE_DISPATCHED_INST);
  STAT_CORE_EVENT_N(m_core_id, CORE_DISPATCH_WAIT, 
      m_cur_core_cycle - cur_uop->m_alloc_cycle);


  // Decrement dispatch m_count for the current thread
  --m_simBase->m_core_pointers[m_core_id]->m_ops_to_be_dispatched[cur_uop->m_thread_id];


  // Uop m_exec ok; update scheduler
  cur_uop->m_in_scheduler = false;
  --m_num_in_sched;
  STAT_CORE_EVENT(m_core_id, POWER_INST_ISSUE_SEL_LOGIC_W);
  STAT_CORE_EVENT(m_core_id, POWER_PAYLOAD_RAM_W);


  switch (q_num) {
    case gen_ALLOCQ : 
      --m_num_per_sched[gen_ALLOCQ]; 
      break;
    case mem_ALLOCQ : 
      --m_num_per_sched[mem_ALLOCQ];  
      break;
    case fp_ALLOCQ : 
      --m_num_per_sched[fp_ALLOCQ]; 
      break;
    default:
     printf("unknown queue\n");
    exit(EXIT_FAILURE);
  }
  
  DEBUG("done schedule core_id:%d thread_id:%d uop_num:%lld inst_num:%lld "
      "entry:%d queue:%d m_num_in_sched:%d m_num_per_sched[general]:%d "
      "m_num_per_sched[mem]:%d m_num_per_sched[fp]:%d done_cycle:%lld\n",
      m_core_id, cur_uop->m_thread_id, cur_uop->m_uop_num,
      cur_uop->m_inst_num, entry, cur_uop->m_allocq_num, m_num_in_sched,
      m_num_per_sched[gen_ALLOCQ], m_num_per_sched[mem_ALLOCQ],
      m_num_per_sched[fp_ALLOCQ], cur_uop->m_done_cycle); 

  return true;
}


// main execution routine
// In every cycle, schedule uops from rob
void schedule_smc_c::run_a_cycle(void) 
{
  // check if the scheduler is running
  if (!is_running()) {
    return;
  }

  m_cur_core_cycle = m_simBase->m_core_cycle[m_core_id];

  // GPU : schedule every N cycles (G80:4, Fermi:2)
  m_schedule_modulo = (m_schedule_modulo + 1) % *m_simBase->m_knobs->KNOB_GPU_SCHEDULE_RATIO;
  if (m_schedule_modulo) 
    return;

  // clear execution port
  m_exec->clear_ports(); 


  // GPU : recent GPUs have dual warp schedulers. In each schedule cycle, each warp scheduler
  // can schedule instructions from different threads. We enforce threads selected by
  // each warp scheduler should be different. 
  int count = 0;
  for (int ii = m_first_schlist; ii != m_last_schlist; ii = (ii + 1) % m_schlist_size) { 
    // -------------------------------------
    // Schedule stops when
    // 1) no uops in the scheduler (m_num_in_sched and first == last)
    // 2) # warp scheduler
    // 3) FIXME add width condition
    // -------------------------------------
    if (!m_num_in_sched || 
        m_first_schlist == m_last_schlist || 
        count == *m_simBase->m_knobs->KNOB_NUM_WARP_SCHEDULER) 
      break;

    SCHED_FAIL_TYPE sched_fail_reason;

    int thread_id = m_schlist_tid[ii];
    int entry = m_schlist_entry[ii];

    bool uop_scheduled = false;
    bool scheduled = false;

    if (entry != -1) {
      // schedule a uop from a thread
      if (uop_schedule(thread_id, entry, &sched_fail_reason)) {
        STAT_CORE_EVENT(m_core_id, SCHED_FAILED_REASON_SUCCESS);

        m_schlist_entry[ii] = -1;
        m_schlist_tid[ii] = -1;
        if (ii == m_first_schlist) {
          m_first_schlist = (m_first_schlist + 1) % m_schlist_size;
        }

        uop_scheduled = true;
        ++count;
      }
      else {
        STAT_CORE_EVENT(m_core_id, 
            SCHED_FAILED_REASON_SUCCESS + MIN2(sched_fail_reason, 2));
      }
    }
  }


  // no uop is scheduled in this cycle
  if (count == 0) {
    STAT_CORE_EVENT(m_core_id, NUM_NO_SCHED_CYCLE);
    STAT_EVENT(AVG_CORE_IDLE_CYCLE);
  }


  // advance entries from alloc queue to schedule queue 
  for (int ii = 0; ii < max_ALLOCQ; ++ii) {
    advance(ii);
  }
}


#if 0
// when a thread terminates, free the sheduler queue allocated to it
void schedule_smc_c::free_sched_queue(int thread_id) 
{
  auto itr = m_thread_to_list_id_map.find(thread_id);
  auto end = m_thread_to_list_id_map.end();
  if (itr != end) {
    m_free_list.push_back(itr->second);
    m_thread_to_list_id_map.erase(itr);
  }
}


// called from : schedule_smc_c::advance()
// returns the index of the scheduler queue assigned to a thread
int schedule_smc_c::get_reserved_sched_queue(int thread_id) 
{
  auto itr = m_thread_to_list_id_map.find(thread_id);
  auto end = m_thread_to_list_id_map.end();

  if (itr != end) {
    return itr->second;
  }
  else {
    return -1;
  }
}


// called from schedule_smc_c::reserve_sched_queue()
// initialize a scheduler queue
void schedule_smc_c::reinit_sched_queue(int entry) 
{
  m_first_schlist_ptrs[entry] = 0;
  m_last_schlist_ptrs[entry]  = 0;
  fill_n(m_schedule_lists[entry], MAX_GPU_SCHED_SIZE, -1); //not required
}


// called from core_c::allocate_core_data()
// when a thread is assigned to a core, a sheduler queue is assigned to it
int schedule_smc_c::reserve_sched_queue(int thread_id) 
{
  auto itr = m_thread_to_list_id_map.find(thread_id);
  auto end = m_thread_to_list_id_map.end();
  if (itr == end) {
    ASSERTM(m_free_list.size(), "size:%d\n", 
        static_cast<int>(m_free_list.size()));
    ASSERT(m_free_list.size());
    int index = m_free_list.front();
    m_free_list.pop_front();

    m_thread_to_list_id_map.insert(pair<int, int>(thread_id, index));
    reinit_sched_queue(index);

    return index;
  }
  return -1; 
}
#endif

