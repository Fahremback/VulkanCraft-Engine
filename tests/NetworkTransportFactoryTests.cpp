#include "engine/networking/INetworkTransportFactory.hpp"
#include <cassert>
#include <iostream>
int main(){auto f=engine::networking::create_network_transport_factory();assert(f);auto b=f->backends();assert(b.size()==3&&b[0].available&&!b[1].available&&!b[2].available);std::string e;auto t=f->create({engine::networking::TransportKind::Loopback,{"127.0.0.1",7777},32,2,false},e);assert(t);assert(t->set_peer_id(42,e)&&t->peer_id()==42);std::cout<<"network-transport-factory-ok\n";}
