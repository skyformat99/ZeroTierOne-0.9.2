/*
 * ZeroTier One - Global Peer to Peer Ethernet
 * Copyright (C) 2011-2014  ZeroTier Networks LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "Constants.hpp"
#include "Network.hpp"
#include "RuntimeEnvironment.hpp"
#include "NodeConfig.hpp"
#include "Switch.hpp"
#include "Packet.hpp"
#include "Buffer.hpp"
#include "EthernetTap.hpp"
#include "EthernetTapFactory.hpp"
#include "RoutingTable.hpp"

#define ZT_NETWORK_CERT_WRITE_BUF_SIZE 131072

namespace ZeroTier {

const ZeroTier::MulticastGroup Network::BROADCAST(ZeroTier::MAC(0xff),0);

const char *Network::statusString(const Status s)
	throw()
{
	switch(s) {
		case NETWORK_INITIALIZING: return "INITIALIZING";
		case NETWORK_WAITING_FOR_FIRST_AUTOCONF: return "WAITING_FOR_FIRST_AUTOCONF";
		case NETWORK_OK: return "OK";
		case NETWORK_ACCESS_DENIED: return "ACCESS_DENIED";
		case NETWORK_NOT_FOUND: return "NOT_FOUND";
		case NETWORK_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
		case NETWORK_NO_MORE_DEVICES: return "NO_MORE_DEVICES";
	}
	return "(invalid)";
}

Network::~Network()
{
	_lock.lock();
	if ((_setupThread)&&(!_destroyed)) {
		_lock.unlock();
		Thread::join(_setupThread);
	} else _lock.unlock();

	{
		Mutex::Lock _l(_lock);
		if (_tap)
			_r->tapFactory->close(_tap,_destroyed);
	}

	if (_destroyed) {
		Utils::rm(std::string(_r->homePath + ZT_PATH_SEPARATOR_S + "networks.d" + ZT_PATH_SEPARATOR_S + idString() + ".conf"));
		Utils::rm(std::string(_r->homePath + ZT_PATH_SEPARATOR_S + "networks.d" + ZT_PATH_SEPARATOR_S + idString() + ".mcerts"));
	} else {
		clean();
		_dumpMulticastCerts();
	}
}

SharedPtr<Network> Network::newInstance(const RuntimeEnvironment *renv,NodeConfig *nc,uint64_t id)
{
	SharedPtr<Network> nw(new Network());
	nw->_id = id;
	nw->_nc = nc;
	nw->_mac.fromAddress(renv->identity.address(),id);
	nw->_r = renv;
	nw->_tap = (EthernetTap *)0;
	nw->_enabled = true;
	nw->_lastConfigUpdate = 0;
	nw->_destroyed = false;
	nw->_netconfFailure = NETCONF_FAILURE_NONE;

	if (nw->controller() == renv->identity.address()) // TODO: fix Switch to allow packets to self
		throw std::runtime_error("cannot join a network for which I am the netconf master");

	try {
		nw->_restoreState();
		nw->requestConfiguration();
	} catch ( ... ) {
		nw->_lastConfigUpdate = 0; // call requestConfiguration() again
	}

	return nw;
}

bool Network::updateMulticastGroups()
{
	Mutex::Lock _l(_lock);
	EthernetTap *t = _tap;
	if (t) {
		// Grab current groups from the local tap
		bool updated = t->updateMulticastGroups(_multicastGroups);

		// Merge in learned groups from any hosts bridged in behind us
		for(std::map<MulticastGroup,uint64_t>::const_iterator mg(_bridgedMulticastGroups.begin());mg!=_bridgedMulticastGroups.end();++mg)
			_multicastGroups.insert(mg->first);

		// Add or remove BROADCAST group based on broadcast enabled netconf flag
		if ((_config)&&(_config->enableBroadcast())) {
			if (_multicastGroups.count(BROADCAST))
				return updated;
			else {
				_multicastGroups.insert(BROADCAST);
				return true;
			}
		} else {
			if (_multicastGroups.count(BROADCAST)) {
				_multicastGroups.erase(BROADCAST);
				return true;
			} else return updated;
		}
	} else return false;
}

bool Network::setConfiguration(const Dictionary &conf,bool saveToDisk)
{
	Mutex::Lock _l(_lock);

	if (_destroyed)
		return false;

	try {
		SharedPtr<NetworkConfig> newConfig(new NetworkConfig(conf)); // throws if invalid
		if ((newConfig->networkId() == _id)&&(newConfig->issuedTo() == _r->identity.address())) {
			_config = newConfig;

			_lastConfigUpdate = Utils::now();
			_netconfFailure = NETCONF_FAILURE_NONE;

			if (saveToDisk) {
				std::string confPath(_r->homePath + ZT_PATH_SEPARATOR_S + "networks.d" + ZT_PATH_SEPARATOR_S + idString() + ".conf");
				if (!Utils::writeFile(confPath.c_str(),conf.toString())) {
					LOG("error: unable to write network configuration file at: %s",confPath.c_str());
				} else {
					Utils::lockDownFile(confPath.c_str(),false);
				}
			}

			EthernetTap *t = _tap;
			if (t) {
				char fname[1024];
				_mkNetworkFriendlyName(fname,sizeof(fname));
				t->setIps(newConfig->staticIps());
				t->setFriendlyName(fname);
			} else {
				if (!_setupThread)
					_setupThread = Thread::start<Network>(this);
			}

			return true;
		} else {
			LOG("ignored invalid configuration for network %.16llx (configuration contains mismatched network ID or issued-to address)",(unsigned long long)_id);
		}
	} catch (std::exception &exc) {
		LOG("ignored invalid configuration for network %.16llx (%s)",(unsigned long long)_id,exc.what());
	} catch ( ... ) {
		LOG("ignored invalid configuration for network %.16llx (unknown exception)",(unsigned long long)_id);
	}

	return false;
}

void Network::requestConfiguration()
{
	if (controller() == _r->identity.address()) {
		// netconf master cannot be a member of its own nets
		LOG("unable to request network configuration for network %.16llx: I am the network master, cannot query self",(unsigned long long)_id);
		return;
	}

	TRACE("requesting netconf for network %.16llx from netconf master %s",(unsigned long long)_id,controller().toString().c_str());
	Packet outp(controller(),_r->identity.address(),Packet::VERB_NETWORK_CONFIG_REQUEST);
	outp.append((uint64_t)_id);
	outp.append((uint16_t)0); // no meta-data
	_r->sw->send(outp,true);
}

void Network::addMembershipCertificate(const CertificateOfMembership &cert)
{
	if (!cert) // sanity check
		return;

	Mutex::Lock _l(_lock);

	// We go ahead and accept certs provisionally even if _isOpen is true, since
	// that might be changed in short order if the user is fiddling in the UI.
	// These will be purged on clean() for open networks eventually.

	CertificateOfMembership &old = _membershipCertificates[cert.issuedTo()];
	if (cert.timestamp() >= old.timestamp()) {
		//TRACE("got new certificate for %s on network %.16llx",cert.issuedTo().toString().c_str(),cert.networkId());
		old = cert;
	}
}

bool Network::isAllowed(const Address &peer) const
{
	try {
		Mutex::Lock _l(_lock);

		if (!_config)
			return false;
		if (_config->isPublic())
			return true;

		std::map<Address,CertificateOfMembership>::const_iterator pc(_membershipCertificates.find(peer));
		if (pc == _membershipCertificates.end())
			return false; // no certificate on file
		return _config->com().agreesWith(pc->second); // is other cert valid against ours?
	} catch (std::exception &exc) {
		TRACE("isAllowed() check failed for peer %s: unexpected exception: %s",peer.toString().c_str(),exc.what());
	} catch ( ... ) {
		TRACE("isAllowed() check failed for peer %s: unexpected exception: unknown exception",peer.toString().c_str());
	}
	return false; // default position on any failure
}

void Network::clean()
{
	Mutex::Lock _l(_lock);

	if (_destroyed)
		return;

	uint64_t now = Utils::now();

	if ((_config)&&(_config->isPublic())) {
		// Open (public) networks do not track certs or cert pushes at all.
		_membershipCertificates.clear();
		_lastPushedMembershipCertificate.clear();
	} else if (_config) {
		// Clean certificates that are no longer valid from the cache.
		for(std::map<Address,CertificateOfMembership>::iterator c=(_membershipCertificates.begin());c!=_membershipCertificates.end();) {
			if (_config->com().agreesWith(c->second))
				++c;
			else _membershipCertificates.erase(c++);
		}

		// Clean entries from the last pushed tracking map if they're so old as
		// to be no longer relevant.
		uint64_t forgetIfBefore = now - (_config->com().timestampMaxDelta() * 3ULL);
		for(std::map<Address,uint64_t>::iterator lp(_lastPushedMembershipCertificate.begin());lp!=_lastPushedMembershipCertificate.end();) {
			if (lp->second < forgetIfBefore)
				_lastPushedMembershipCertificate.erase(lp++);
			else ++lp;
		}
	}

	// Clean learned multicast groups if we haven't heard from them in a while
	for(std::map<MulticastGroup,uint64_t>::iterator mg(_bridgedMulticastGroups.begin());mg!=_bridgedMulticastGroups.end();) {
		if ((now - mg->second) > (ZT_MULTICAST_LIKE_EXPIRE * 2))
			_bridgedMulticastGroups.erase(mg++);
		else ++mg;
	}
}

Network::Status Network::status() const
{
	Mutex::Lock _l(_lock);
	switch(_netconfFailure) {
		case NETCONF_FAILURE_ACCESS_DENIED:
			return NETWORK_ACCESS_DENIED;
		case NETCONF_FAILURE_NOT_FOUND:
			return NETWORK_NOT_FOUND;
		case NETCONF_FAILURE_NONE:
			return ((_lastConfigUpdate > 0) ? ((_tap) ? NETWORK_OK : NETWORK_INITIALIZING) : NETWORK_WAITING_FOR_FIRST_AUTOCONF);
		//case NETCONF_FAILURE_INIT_FAILED:
		default:
			return NETWORK_INITIALIZATION_FAILED;
	}
}

void Network::_CBhandleTapData(void *arg,const MAC &from,const MAC &to,unsigned int etherType,const Buffer<4096> &data)
{
	if ((!((Network *)arg)->_enabled)||(((Network *)arg)->status() != NETWORK_OK))
		return;

	const RuntimeEnvironment *_r = ((Network *)arg)->_r;
	if (_r->shutdownInProgress)
		return;

	try {
		_r->sw->onLocalEthernet(SharedPtr<Network>((Network *)arg),from,to,etherType,data);
	} catch (std::exception &exc) {
		TRACE("unexpected exception handling local packet: %s",exc.what());
	} catch ( ... ) {
		TRACE("unexpected exception handling local packet");
	}
}

void Network::_pushMembershipCertificate(const Address &peer,bool force,uint64_t now)
{
	uint64_t pushTimeout = _config->com().timestampMaxDelta() / 2;
	if (!pushTimeout)
		return; // still waiting on my own cert
	if (pushTimeout > 1000)
		pushTimeout -= 1000;

	uint64_t &lastPushed = _lastPushedMembershipCertificate[peer];
	if ((force)||((now - lastPushed) > pushTimeout)) {
		lastPushed = now;
		TRACE("pushing membership cert for %.16llx to %s",(unsigned long long)_id,peer.toString().c_str());

		Packet outp(peer,_r->identity.address(),Packet::VERB_NETWORK_MEMBERSHIP_CERTIFICATE);
		_config->com().serialize(outp);
		_r->sw->send(outp,true);
	}
}

// Ethernet tap creation thread -- required on some platforms where tap
// creation may be time consuming (e.g. Windows).
void Network::threadMain()
	throw()
{
	char fname[1024],lcentry[128];
	Utils::snprintf(lcentry,sizeof(lcentry),"_dev_for_%.16llx",(unsigned long long)_id);

	EthernetTap *t = (EthernetTap *)0;
	try {
		std::string desiredDevice(_nc->getLocalConfig(lcentry));
		_mkNetworkFriendlyName(fname,sizeof(fname));

		t = _r->tapFactory->open(_mac,ZT_IF_MTU,ZT_DEFAULT_IF_METRIC,_id,(desiredDevice.length() > 0) ? desiredDevice.c_str() : (const char *)0,fname,_CBhandleTapData,this);

		std::string dn(t->deviceName());
		if ((dn.length())&&(dn != desiredDevice))
			_nc->putLocalConfig(lcentry,dn);
	} catch (std::exception &exc) {
		delete t;
		t = (EthernetTap *)0;
		LOG("network %.16llx failed to initialize: %s",_id,exc.what());
		_netconfFailure = NETCONF_FAILURE_INIT_FAILED;
	} catch ( ... ) {
		delete t;
		t = (EthernetTap *)0;
		LOG("network %.16llx failed to initialize: unknown error",_id);
		_netconfFailure = NETCONF_FAILURE_INIT_FAILED;
	}

	{
		Mutex::Lock _l(_lock);
		if (_tap) // the tap creation thread can technically be re-launched, though this isn't done right now
			_r->tapFactory->close(_tap,false);
		_tap = t;
		if (t) {
			if (_config)
				t->setIps(_config->staticIps());
			t->setEnabled(_enabled);
		}
	}
}

void Network::learnBridgeRoute(const MAC &mac,const Address &addr)
{
	Mutex::Lock _l(_lock);
	_bridgeRoutes[mac] = addr;

	// If _bridgeRoutes exceeds sanity limit, trim worst offenders until below -- denial of service circuit breaker
	while (_bridgeRoutes.size() > ZT_MAX_BRIDGE_ROUTES) {
		std::map<Address,unsigned long> counts;
		Address maxAddr;
		unsigned long maxCount = 0;
		for(std::map<MAC,Address>::iterator br(_bridgeRoutes.begin());br!=_bridgeRoutes.end();++br) {
			unsigned long c = ++counts[br->second];
			if (c > maxCount) {
				maxCount = c;
				maxAddr = br->second;
			}
		}
		for(std::map<MAC,Address>::iterator br(_bridgeRoutes.begin());br!=_bridgeRoutes.end();) {
			if (br->second == maxAddr)
				_bridgeRoutes.erase(br++);
			else ++br;
		}
	}
}

void Network::setEnabled(bool enabled)
{
	Mutex::Lock _l(_lock);
	_enabled = enabled;
	if (_tap)
		_tap->setEnabled(enabled);
}

void Network::destroy()
{
	Mutex::Lock _l(_lock);

	_enabled = false;
	_destroyed = true;

	if (_setupThread)
		Thread::join(_setupThread);
	_setupThread = Thread();

	if (_tap)
		_r->tapFactory->close(_tap,true);
	_tap = (EthernetTap *)0;
}

void Network::_restoreState()
{
	Buffer<ZT_NETWORK_CERT_WRITE_BUF_SIZE> buf;

	std::string idstr(idString());
	std::string confPath(_r->homePath + ZT_PATH_SEPARATOR_S + "networks.d" + ZT_PATH_SEPARATOR_S + idstr + ".conf");
	std::string mcdbPath(_r->homePath + ZT_PATH_SEPARATOR_S + "networks.d" + ZT_PATH_SEPARATOR_S + idstr + ".mcerts");

	// Read configuration file containing last config from netconf master
	{
		std::string confs;
		if (Utils::readFile(confPath.c_str(),confs)) {
			try {
				if (confs.length())
					setConfiguration(Dictionary(confs),false);
			} catch ( ... ) {} // ignore invalid config on disk, we will re-request from netconf master
		} else {
			// If the conf file isn't present, "touch" it so we'll remember
			// the existence of this network.
			FILE *tmp = fopen(confPath.c_str(),"w");
			if (tmp)
				fclose(tmp);
		}
	}

	// Read most recent multicast cert dump
	if ((_config)&&(!_config->isPublic())&&(Utils::fileExists(mcdbPath.c_str()))) {
		CertificateOfMembership com;
		Mutex::Lock _l(_lock);

		_membershipCertificates.clear();

		FILE *mcdb = fopen(mcdbPath.c_str(),"rb");
		if (mcdb) {
			try {
				char magic[6];
				if ((fread(magic,6,1,mcdb) == 1)&&(!memcmp("ZTMCD0",magic,6))) {
					long rlen = 0;
					do {
						long rlen = (long)fread(buf.data() + buf.size(),1,ZT_NETWORK_CERT_WRITE_BUF_SIZE - buf.size(),mcdb);
						if (rlen < 0) rlen = 0;
						buf.setSize(buf.size() + (unsigned int)rlen);
						unsigned int ptr = 0;
						while ((ptr < (ZT_NETWORK_CERT_WRITE_BUF_SIZE / 2))&&(ptr < buf.size())) {
							ptr += com.deserialize(buf,ptr);
							if (com.issuedTo())
								_membershipCertificates[com.issuedTo()] = com;
						}
						if (ptr) {
							memmove(buf.data(),buf.data() + ptr,buf.size() - ptr);
							buf.setSize(buf.size() - ptr);
						}
					} while (rlen > 0);
					fclose(mcdb);
				} else {
					fclose(mcdb);
					Utils::rm(mcdbPath);
				}
			} catch ( ... ) {
				// Membership cert dump file invalid. We'll re-learn them off the net.
				_membershipCertificates.clear();
				fclose(mcdb);
				Utils::rm(mcdbPath);
			}
		}
	}
}

void Network::_dumpMulticastCerts()
{
	Buffer<ZT_NETWORK_CERT_WRITE_BUF_SIZE> buf;
	std::string mcdbPath(_r->homePath + ZT_PATH_SEPARATOR_S + "networks.d" + ZT_PATH_SEPARATOR_S + idString() + ".mcerts");
	Mutex::Lock _l(_lock);

	if (!_config)
		return;

	if ((!_id)||(_config->isPublic())) {
		Utils::rm(mcdbPath);
		return;
	}

	FILE *mcdb = fopen(mcdbPath.c_str(),"wb");
	if (!mcdb)
		return;
	if (fwrite("ZTMCD0",6,1,mcdb) != 1) {
		fclose(mcdb);
		Utils::rm(mcdbPath);
		return;
	}

	for(std::map<Address,CertificateOfMembership>::iterator c=(_membershipCertificates.begin());c!=_membershipCertificates.end();++c) {
		try {
			c->second.serialize(buf);
			if (buf.size() >= (ZT_NETWORK_CERT_WRITE_BUF_SIZE / 2)) {
				if (fwrite(buf.data(),buf.size(),1,mcdb) != 1) {
					fclose(mcdb);
					Utils::rm(mcdbPath);
					return;
				}
				buf.clear();
			}
		} catch ( ... ) {
			// Sanity check... no cert will ever be big enough to overflow buf
			fclose(mcdb);
			Utils::rm(mcdbPath);
			return;
		}
	}

	if (buf.size()) {
		if (fwrite(buf.data(),buf.size(),1,mcdb) != 1) {
			fclose(mcdb);
			Utils::rm(mcdbPath);
			return;
		}
	}

	fclose(mcdb);
	Utils::lockDownFile(mcdbPath.c_str(),false);
}

} // namespace ZeroTier
