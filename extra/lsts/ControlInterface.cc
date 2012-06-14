#include "ControlInterface.hh"

// POSIX headers for fifo pipes
#include <sys/types.h>
#include <sys/stat.h>
#include <extra/lsts/Platform.hh>

#include <sstream>

using namespace TREX::LSTS;
using namespace TREX::transaction;
using namespace TREX::utils;

namespace bfs=boost::filesystem;
namespace xml = boost::property_tree::xml_parser;

namespace
{
  
  /** @brief GoalPipe reactor declaration */
  TeleoReactor::xml_factory::declare<ControlInterface> decl("GoalPipe");

}

/*
 * class TREX::LSTS::ControlInterface::thread_proxy
 */ 

// structors

ControlInterface::thread_proxy::thread_proxy(thread_proxy const &other)
:m_reactor(other.m_reactor) {}

ControlInterface::thread_proxy::thread_proxy(ControlInterface *me) 
:m_reactor(me) {}

ControlInterface::thread_proxy::~thread_proxy() {}
  
void ControlInterface::thread_proxy::operator()() {
  if( NULL!=m_reactor && !m_reactor->is_running() )
    m_reactor->run();
}
  


/*
 * class TREX::LSTS::ControlInterface
 */ 

// statics 

SharedVar<size_t> ControlInterface::s_id;

// structors 

ControlInterface::ControlInterface(TREX::transaction::TeleoReactor::xml_arg_type arg)
:TeleoReactor(arg, false), m_running(false), m_fifo(0) {}

ControlInterface::~ControlInterface() {
  stop();
  destroy_fifo();
}

// observers

std::string ControlInterface::fifo_name() const {
  std::ostringstream oss;
  // file name is <agent>.<reactor>.in
  oss<<getAgentName()<<'.'<<getName()<<".in";
  // Position the file into our log directory
  return manager().file_name(oss.str()).string();
}

bool ControlInterface::is_running() const {
  scoped_lock cs(m_mutex);
  return m_running;
}

bool ControlInterface::is_open() const {
  scoped_lock cs(m_mutex);
  return 0<m_fifo;  
}


// manipulators

void ControlInterface::add_goal(goal_id const &g) {
  {
    scoped_lock cs;
    m_pending_goals.push_back(g);
  }
  syslog()<<"Goal ["<<g<<"] added to pending queue :\n\t"<<*g;
}


bool ControlInterface::next_goal(goal_id &g) {
  scoped_lock cs(m_mutex);
  if( m_pending_goals.empty() )
    return false;
  else {
    g = m_pending_goals.front();
    m_pending_goals.pop_front();
    return true;
  }
}


std::string ControlInterface::log_message(std::string const &content) {
  std::ostringstream name;
  // Build unique message name
  name<<"msg.";
  {
    SharedVar<size_t>::scoped_lock cs(s_id);
    name<<(++(*s_id))<<".rcvd";
  }
  // Log the message
  std::string full_name = manager().file_name(name.str()).string();
  std::ofstream out(full_name.c_str(), std::ofstream::binary);
  out.write(content.c_str(), content.length());
  return name.str();
}

void ControlInterface::create_fifo() {
  int fid;
  
  { // critical section
    scoped_lock cs(m_mutex);
    // Get the file id 
    fid = m_fifo;
  }
    
  
  if( 0==fid ) {
    std::string queue_name = fifo_name();
    syslog()<<"Creating fifo pipe \""<<queue_name<<"\" ...";
    int ret = mkfifo(queue_name.c_str(), S_IWUSR|S_IWGRP|S_IRUSR|S_IRGRP);
    
    if( 0!=ret ) {
      if( EEXIST==errno ) {
        syslog("WARNING")<<"A file with this name did already exist !!!"
                         <<"\n\tI'll assume it is a unix pipe.";
      } else {
        syslog("ERROR")<<"Failed to create fifo";
        throw TREX::utils::ErrnoExcept("mkfifo("+queue_name+")");
      }
    }
    syslog()<<"Opening the pipe...";
    fid = open(queue_name.c_str(), O_RDONLY|O_NONBLOCK);
    if( fid<0 ) {
      syslog("ERROR")<<"Failed to open fifo";
      throw TREX::utils::ErrnoExcept("open("+queue_name+")");
    } else { // critical section
      scoped_lock cs(m_mutex);
      // update the file id to the newly opened fifo
      m_fifo = fid;
    }
  } 
}

void ControlInterface::destroy_fifo() {
  int fid = 0;
  
  { // critical section
    scoped_lock cs(m_mutex);
    // get file id and reset it to 0
    std::swap(fid, m_fifo);
  }
  
  if( 0<fid ) {
    close(fid);
    // remove the queue from the file system
    bfs::path fpath(fifo_name());
    bfs::remove(fpath);
  }
}

size_t ControlInterface::retrieve_from_fifo(char *buff, size_t buff_size, int us_timer) {
  int fid;
  {
    scoped_lock cs(m_mutex);
    fid = m_fifo; 
  }
  if( 0<fid ) {
    struct timeval t_out;
    fd_set files;
    int ret;
    // do a slect on our fid
    t_out.tv_sec = 0;
    t_out.tv_usec = us_timer;
    FD_ZERO(&files);
    FD_SET(fid, &files);
    ret = select(fid+1, &files, NULL, NULL, &t_out);
    
    if( ret>0 && FD_ISSET(fid, &files) ) {
      // read from the pipe if still open
      scoped_lock cs(m_mutex);
      if( 0<m_fifo ) {
        int len = read(m_fifo, buff, buff_size*sizeof(char));
        
        if( len<0 )
          throw TREX::utils::ErrnoExcept("Error while reading fifo");
        return len;
      }
    }
  }
  return 0;
}

void ControlInterface::proccess_message(std::string const &msg) {  
  // First log the message
  std::string msg_id = log_message(msg);
  syslog("INFO")<<"Received message "<<msg_id;
  
  boost::property_tree::ptree xml_tree;
  try {
    std::istringstream is(msg);
    read_xml(is, xml_tree, xml::no_comments|xml::trim_whitespace);
  } catch(xml::xml_parser_error const &e) {
    syslog("WARN")<<"Xml error while parsing "<<msg_id<<":\n\t"<<e.what();
    return;
  }
  
  // Load all the <Goal>
  boost::property_tree::ptree::assoc_iterator i, last;
  
  boost::tie(i,last) = xml_tree.equal_range("Goal");
  if( last==i ) 
    syslog("WARN")<<"No <Goal> in "<<msg_id;
  else {
    for( ; last!=i; ++i) {
      try {
        goal_id tmp(new Goal(*i));
        add_goal(tmp);
      } catch(TREX::utils::Exception const &e) {
        syslog("INFO")<<"Exception while building new goal: "<<e; 
      }
    }
  }
}

void ControlInterface::stop() {
  scoped_lock cs(m_mutex);
  m_running = false;
}

// TREX callbacks 

void ControlInterface::handleInit() {
  // Create the new fifo queue
  create_fifo();
  // spawn my listener thread 
  m_thread.reset(new boost::thread(thread_proxy(this)));
  Platform::setControlInterface(this);
}

void ControlInterface::handleTickStart() {
  goal_id g;
  
  // get pending goals
  while( next_goal(g) ) {
    // First attempt to subscribe to the timeline
    if( !isExternal(g->object()) )
      use(g->object());
    if( isExternal(g->object()) )
      postGoal(g);
    else 
      syslog("WARN")<<"Unable to subscribe to timeline \""<<g->object()<<"\".";
  }
}

bool ControlInterface::synchronize() {
  // Nothing to do here 
  return true; 
}

void ControlInterface::run() {
  {
    scoped_lock cs(m_mutex);
    m_running = true;
  }
  try {
    char buff[4096];
    size_t const max_len(4096);
    size_t len;
    int timer = 20; // wait 20 us  
    
    while( is_running() && is_open() ) {
      // I'll assume that data is a string with no '\0' in between
      std::string msg;
      
      // get all the data in the pipe
      do {
        len = retrieve_from_fifo(buff, max_len, timer);
        msg.insert(msg.end(), buff, buff+len);
      } while( len>0 );
      
      // check if any data received
      if( !msg.empty() ) {
        syslog()<<"Received "<<msg.size()<<" bytes."; 
        proccess_message(msg);
      }
      // sleep a little
      boost::thread::yield();
    }
  } catch(TREX::utils::Exception const &te) {
    syslog("ERROR")<<"In fifo listener thread: "<<te;
  } catch(std::exception const &se) {
    syslog("ERROR")<<"In fifo listener thread: "<<se.what();
  } catch(...) {
    syslog("ERROR")<<"In fifo listener thread: Unknown exception caught";
  }
  { // critical section : ensure that running is false
    scoped_lock cs(m_mutex);
    m_running = false;
  }
}

