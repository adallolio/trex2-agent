/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2015, Frederic Py.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the TREX Project nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */
#include "ros_reactor.hh"
#include <trex/python/python_thread.hh>
#include <trex/domain/FloatDomain.hh>
#include <trex/domain/IntegerDomain.hh>
#include <trex/domain/BooleanDomain.hh>
#include <trex/domain/StringDomain.hh>

#include <boost/python/def.hpp>
#include <boost/python/stl_iterator.hpp>



using namespace TREX::ROS;
using namespace TREX::utils;
using namespace TREX::transaction;
using namespace TREX::python;
namespace bp=boost::python;
namespace bpt=boost::property_tree;

// structors

ros_reactor::ros_reactor(TeleoReactor::xml_arg_type arg)
:TeleoReactor(arg, false) {
  bpt::ptree::value_type &node(xml_factory::node(arg));
  typedef tl_factory::iter_traits<bpt::ptree::iterator> itt;
  bpt::ptree::iterator ixml = node.second.begin();
  ros_reactor *me = this;
  
  itt::type i = itt::build(ixml, me);
  SHARED_PTR<details::ros_timeline> tl;
  size_t count = 0;
  
  while( m_factory->iter_produce(i, node.second.end(), tl) ) {
    std::pair<tl_set::iterator, bool> ret = m_timelines.insert(tl);
    if( ret.second )
      syslog(null, info)<<"ROS timleine "<<tl->name()<<" created.";
    else
      // should not happen
      throw MultipleInternals(*this, tl->name(), *this);
    ++count;
  }
  
  if( 0==count )
    syslog(log::warn)<<"No connection created: this reactor is useless";
}

ros_reactor::~ros_reactor() {}


// modifiers

//void ros_reactor::add_topic(bpt::ptree::value_type const &desc) {
//  Symbol tl_name = parse_attr<Symbol>(desc, "name");
//  std::string ros_topic = parse_attr<std::string>(desc, "topic");
//  boost::optional<std::string> py_type = parse_attr< boost::optional<std::string> >(desc, "type");
//  bool write = parse_attr<bool>(false, desc, "accept_goals");
//  
//  if( !py_type )
//    py_type = "genpy.message.Message";
//  
//  // First reserve the timeline
//  provide(tl_name, write);
//  if( isInternal(tl_name) ) {
//    scoped_gil_release lock;
//    try {
//      bp::object type = m_python->load_module_for(*py_type);
//      std::string f_name(tl_name.str()+"_updated");
//      {
//        bp::scope local(m_env);
//
//        syslog()<<"Create python callback "<<f_name<<" for topic "<<ros_topic;
//          bp::def(f_name.c_str(),
//                  bp::make_function(boost::bind(&ros_reactor::ros_update,
//                                                this, tl_name, _1),
//                                    bp::default_call_policies(),
//                                    boost::mpl::vector<void, bp::object>()));
//        
//        syslog()<<"Subscribe to topic "<<ros_topic;
//
//        bp::object cb = m_env.attr(f_name.c_str());
//        bp::object sub = m_ros->rospy().attr("Subscriber");
//        
//        m_env.attr((tl_name.str()+"_sub").c_str()) = sub(ros_topic.c_str(), type, cb);
//        syslog()<<"done: "<<f_name<<"="
//          <<bp::extract<char const *>(bp::str(m_env.attr((tl_name.str()+"_sub").c_str())));
//      }
//    
//    } catch(bp::error_already_set const &e) {
//      m_exc->unwrap_py_error();
//    }
//  } else
//    throw XmlError(desc, "Failed to declare \""+tl_name.str()+"\" as internal");
//}
//
//// manipulators
//
//size_t ros_reactor::populate_attributes(Predicate &pred, std::string path,
//                                        bp::object const &obj) {
//  size_t ret = 0;
//  bp::dict type(m_python->dir(obj.attr("__class__")));
//  
//
//  for(bp::stl_input_iterator<bp::str> i(m_python->dir(obj)), end;
//      end!=i; ++i) {
//    if( !(i->startswith("_") || (*i)=="header" || (*i)=="child_frame_id"
//          // check if not static
//          || type.contains(*i)) ) {
//      bp::object const &attr = obj.attr(*i);
//      if( !m_python->callable(attr)  ) {
//        std::string local = path;
//        local += bp::extract<char const *>(*i);
//        
//
//        
//        if( PyString_Check(attr.ptr()) ){
//          std::string val((bp::extract<char const *>(attr)));
//          pred.restrictAttribute(local, StringDomain(val));
//          ++ret;
//        } else {
//          bp::extract<FloatDomain::base_type> is_float(attr);
//          bp::extract<IntegerDomain::base_type> is_int(attr);
//          bp::extract<bool> is_bool(attr);
//        
//          // Order of type identification
//          //  - float vs int do not matter as they are different types in python
//          //  - bool vs int though does matter as bool is a subclass of int in python
//          //  Need to handle enums but this is not exisiting by default in python 2.7
//          // and just appeared in 3.x (also I do not know if ROS will implement enums
//          // as expected)
//          if( is_float.check() ) {
//            syslog()<<local<<" is float";
//            //          if( is_int.check() )
//            //            syslog()<<local<<" is int too *sigh*";
//            pred.restrictAttribute(Variable(local, FloatDomain(is_float)));
//            
//            ++ret;
//          } else if( is_bool.check() ) {
//            syslog()<<local<<" is bool";
//            pred.restrictAttribute(Variable(local, BooleanDomain(is_bool)));
//          } else if ( is_int.check() ) {
//            syslog()<<local<<" is int";
//            // TODO: check for enum if possible
//            pred.restrictAttribute(Variable(local, IntegerDomain(is_int)));
//            ++ret;
//          } else if( m_python->is_instance(attr, m_ros_time) ){
//            // what is the date
//            long double value = bp::extract<long double>(attr.attr("to_time")()),
//            // what is now
//            now = bp::extract<long double>(m_ros_time.attr("now")().attr("to_time")());
//            // what is  the date in trex
//            TICK date = getCurrentTick();
//            // how far ago is the date from now
//            CHRONO::duration<long double> tmp(value-now);
//            duration_type delta = CHRONO::duration_cast<duration_type>(tmp);
//            // add this delta to the current tick
//            date += delta.count()/tickDuration().count();
//            // restrict the variable to the identified tick
//            pred.restrictAttribute(local, IntegerDomain(date));
//          } else if( m_python->is_instance(attr, m_ros_duration) ){
//            // Not sur about this as trex lose the meaning of it
//            // although if I do a cproper message converter then I will
//            // always know the back and forth
//            CHRONO::duration<long double> val(bp::extract<long double>(attr.attr("to_sec")()));
//            duration_type dur = CHRONO::duration_cast<duration_type>(val);
//            TICK value = dur.count()/tickDuration().count();
//            pred.restrictAttribute(local, IntegerDomain(value));
//          } else {
//            // this iterate through structures and seems to avoid
//            // types such as list etc
//            size_t sub = populate_attributes(pred, local+"_", attr);
//            
//            if( 0==sub ) {
//              // This is not a structure nor a basic type
//              syslog()<<"Not able to convertto trex: "<<pred.object()
//                <<'.'<<pred.predicate()<<'.'<<path
//              <<"="<<bp::extract<char const *>(bp::str(attr));
//            } else
//              ret += sub;
//          }
//        }
//      }
//    }
//  }
//  return ret;
//}


// callbacks

void ros_reactor::handleInit() {
  
}

bool ros_reactor::synchronize() {
  return true;
}

//void ros_reactor::ros_update(Symbol timeline, bp::object obj) {
//  python::scoped_gil_release lock;
//  std::string pred;
//  // Extract the type for predicate name
//  if( m_python->dir(obj).contains("_type") ) {
//    pred = bp::extract<char const *>(obj.attr("_type"));
//    size_t pos = pred.find_last_of("./");
//    if( std::string::npos!=pos ) {
//      pred = pred.substr(pos+1);
//    }
//  }
//  
//  Observation obs(timeline, pred);
//  populate_attributes(obs, "", obj);
//  postObservation(obs);
//}

