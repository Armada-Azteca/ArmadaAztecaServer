//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////////////////
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//////////////////////////////////////////////////////////////////////
#include "otpch.h"

#include "outputmessage.h"
#include "server.h"
#include "connection.h"
#include "scheduler.h"
#include "configmanager.h"
#include "ban.h"

extern ConfigManager g_config;
extern Ban g_bans;

ServiceManager::ServiceManager()
	: m_io_service(), death_timer(m_io_service), running(false)
{
	//
}

ServiceManager::~ServiceManager()
{
	stop();
}

std::list<uint16_t> ServiceManager::get_ports() const
{
	std::list<uint16_t> ports;
	for(std::map<uint16_t, ServicePort_ptr>::const_iterator it = m_acceptors.begin();
		it != m_acceptors.end(); ++it)
		ports.push_back(it->first);

	ports.unique();
	return ports;
}

void ServiceManager::die()
{
	m_io_service.stop();
}

void ServiceManager::run()
{
	assert(!running);
	running = true;
	m_io_service.run();
}

void ServiceManager::stop()
{
	if(!running)
		return;

	running = false;

	for(std::map<uint16_t, ServicePort_ptr>::iterator it = m_acceptors.begin();
		it != m_acceptors.end(); ++it)
	{
		try {
			m_io_service.post(boost::bind(&ServicePort::onStopServer, it->second));
		} catch (boost::system::system_error& e) {
			std::cout << "[ServiceManager::stop] Network Error: " << e.what() << std::endl;
		}
	}
	m_acceptors.clear();

	OutputMessagePool::getInstance()->stop();

	death_timer.expires_from_now(boost::posix_time::seconds(3));
	death_timer.async_wait(boost::bind(&ServiceManager::die, this));
}

ServicePort::ServicePort(boost::asio::io_service& io_service) :
	m_io_service(io_service),
	m_acceptor(NULL),
	m_serverPort(0),
	m_pendingStart(false)
{
	//
}

ServicePort::~ServicePort()
{
	close();
}

bool ServicePort::is_single_socket() const
{
	return m_services.size() && m_services.front()->is_single_socket();
}

std::string ServicePort::get_protocol_names() const
{
	if(m_services.empty())
		return "";

	std::string str = m_services.front()->get_protocol_name();
	for(uint32_t i = 1; i < m_services.size(); ++i)
	{
		str += ", ";
		str += m_services[i]->get_protocol_name();
	}
	return str;
}

void ServicePort::accept()
{
	if(!m_acceptor)
	{
		#ifdef __DEBUG_NET__
		std::cout << "Error: [ServicePort::accept] NULL m_acceptor." << std::endl;
		#endif
		return;
	}

	boost::asio::ip::tcp::socket* socket = new boost::asio::ip::tcp::socket(m_io_service);

	m_acceptor->async_accept(*socket,
		boost::bind(&ServicePort::onAccept, this, socket,
		boost::asio::placeholders::error));
}

void ServicePort::onAccept(boost::asio::ip::tcp::socket* socket, const boost::system::error_code& error)
{
	if(!error)
	{
		if(m_services.empty())
		{
#ifdef __DEBUG_NET__
			std::cout << "Error: [ServerPort::accept] No services running!" << std::endl;
#endif
			return;
		}

		boost::system::error_code error;
		const boost::asio::ip::tcp::endpoint endpoint = socket->remote_endpoint(error);
		uint32_t remote_ip = 0;
		if(!error)
			remote_ip = htonl(endpoint.address().to_v4().to_ulong());

		if(remote_ip != 0 && g_bans.acceptConnection(remote_ip))
		{
			Connection_ptr connection = ConnectionManager::getInstance()->createConnection(socket, m_io_service, shared_from_this());
			Service_ptr service = m_services.front();
			if(service->is_single_socket())
				connection->acceptConnection(service->make_protocol(connection));
			else
				connection->acceptConnection();
		}
		else if(socket->is_open())
		{
			boost::system::error_code error;
			socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
			socket->close(error);
			delete socket;
		}
		accept();
	}
	else if(error != boost::asio::error::operation_aborted)
	{
		if(!m_pendingStart)
		{
			close();
			m_pendingStart = true;
			g_scheduler.addEvent(createSchedulerTask(15000,
				boost::bind(&ServicePort::openAcceptor, boost::weak_ptr<ServicePort>(shared_from_this()), m_serverPort)));
		}
	}
	else
	{
		#ifdef __DEBUG_NET__
		std::cout << "Error: [ServicePort::onAccept] Operation aborted." << std::endl;
		#endif
	}
}

Protocol* ServicePort::make_protocol(bool checksummed, NetworkMessage& msg) const
{
	uint8_t protocolID = msg.GetByte();
	for(std::vector<Service_ptr>::const_iterator service_iter = m_services.begin(); service_iter != m_services.end(); ++service_iter)
	{
		Service_ptr service = *service_iter;
		if(protocolID != service->get_protocol_identifier())
			continue;

		if((checksummed && service->is_checksummed()) || !service->is_checksummed())
			return service->make_protocol(Connection_ptr());
	}
	return NULL;
}

void ServicePort::onStopServer()
{
	close();
}

void ServicePort::openAcceptor(boost::weak_ptr<ServicePort> weak_service, uint16_t port)
{
	if(weak_service.expired())
		return;

	if(ServicePort_ptr service = weak_service.lock())
		service->open(port);
}

void ServicePort::open(uint16_t port)
{
	close();

	m_serverPort = port;
	m_pendingStart = false;

	try
	{
		if(g_config.getBoolean(ConfigManager::BIND_ONLY_GLOBAL_ADDRESS))
		{
			m_acceptor = new boost::asio::ip::tcp::acceptor(m_io_service, boost::asio::ip::tcp::endpoint(
				boost::asio::ip::address(boost::asio::ip::address_v4::from_string(g_config.getString(ConfigManager::IP))), m_serverPort));
		}
		else
		{
			m_acceptor = new boost::asio::ip::tcp::acceptor(m_io_service, boost::asio::ip::tcp::endpoint(
				boost::asio::ip::address(boost::asio::ip::address_v4(INADDR_ANY)), m_serverPort));
		}

		m_acceptor->set_option(boost::asio::ip::tcp::no_delay(true));

		accept();
	}
	catch(boost::system::system_error& e)
	{
		std::cout << "[ServicePort::open] Error: " << e.what() << std::endl;

		m_pendingStart = true;
		g_scheduler.addEvent(createSchedulerTask(15000,
			boost::bind(&ServicePort::openAcceptor, boost::weak_ptr<ServicePort>(shared_from_this()), port)));
	}
}

void ServicePort::close()
{
	if(m_acceptor)
	{
		if(m_acceptor->is_open())
		{
			boost::system::error_code error;
			m_acceptor->close(error);
			if(error)
			{
				PRINT_ASIO_ERROR("Closing listen socket");
			}
		}
		delete m_acceptor;
		m_acceptor = NULL;
	}
}

bool ServicePort::add_service(Service_ptr new_svc)
{
	for(std::vector<Service_ptr>::const_iterator svc_iter = m_services.begin(); svc_iter != m_services.end(); ++svc_iter)
	{
		Service_ptr svc = *svc_iter;
		if(svc->is_single_socket())
			return false;
	}

	m_services.push_back(new_svc);
	return true;
}
