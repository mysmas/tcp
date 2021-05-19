/*
** 2020
**
** Haining
**
*************************************************************************
**
*/

#pragma once

#include <map>
#include <queue>
#include <chrono>

#include "../3rd/asio-1.16.0/include/asio.hpp"

namespace Tcp
{
	const int server_max_buf_size = 1024 * 500;
	const int client_max_buf_size = 1024 * 500;

	typedef struct _protocol_head
	{
		int size;
	}protocol_head;

	class client_data :public std::enable_shared_from_this<client_data>
	{
	public:
		client_data() = delete;
		client_data(const std::string& id)
			:m_id(id)
			, m_receive_count(0) {
			m_buf.fill('\0');
		};
		~client_data() {};
	public:
		std::string m_id;
		std::array<char, server_max_buf_size> m_buf;
		std::queue <std::shared_ptr<std::vector<char>>> m_send_queue;
		std::chrono::system_clock::time_point m_connect_time_point;
		std::chrono::system_clock::time_point m_receive_time_point;
		std::chrono::system_clock::time_point m_send_time_point;
		std::size_t m_receive_count;
		std::size_t m_receive_count_last;
		std::size_t m_send_count;
		std::size_t m_send_count_last;
	};

	std::vector<char> PackData(const char* buf, const int& size)
	{
		std::vector<char> data;
		protocol_head head;
		head.size = sizeof(protocol_head) + size;
		data.insert(data.end(), (char*)(&head), (char*)(&head) + sizeof(protocol_head));
		data.insert(data.end(), buf, buf + size);
		return std::move(data);
	}

	std::string GetId(std::shared_ptr<asio::ip::tcp::socket> s)
	{
		if (s)
			return  s->remote_endpoint().address().to_string() + ":" + std::to_string(s->remote_endpoint().port());
		else
			return std::string();
	}

	class CTcpServer
	{
	public:
		CTcpServer() = delete;
		CTcpServer(const std::string& ip = "",
			const std::string& port = "8000",
			std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client)> accept_client_callback_fun = nullptr,
			std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, const std::string& error)> close_client_callback_fun = nullptr,
			std::function<bool(std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data, const char* buf, const int& size)> async_send_fun, std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data, const char* buf, const int& size)> handle_buf_callback_fun = nullptr,
			std::function<bool(std::chrono::milliseconds& milliseconds, std::shared_ptr<std::map<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<client_data>>>)> timer_callback_fun = nullptr,
			std::function<bool(const std::string& msg)> log_callback_fun = nullptr)
			: m_ip(ip)
			, m_port(port)
			, m_appEnable(true)
			, m_strand(m_io_context_)
			, m_accept_client_callback_fun(accept_client_callback_fun)
			, m_close_client_callback_fun(close_client_callback_fun)
			, m_deal_buf_callback_fun(handle_buf_callback_fun)
			, m_timer_callback_fun(timer_callback_fun)
			, m_log_callback_fun(m_log_callback_fun)
			, m_check_client_timer(m_io_context_)
			, m_timer(m_io_context_)
		{
		};

		~CTcpServer() {
			m_io_context_.post([this]()
				{
					m_appEnable = false;

					m_acceptor_->close();

					__delete_client();

					m_work.reset();
				});

			for (auto& t : m_threads)
			{
				t.join();
			}
		};
	public:
		bool run() {
			try
			{
				m_acceptor_ = std::make_shared<asio::ip::tcp::acceptor>(m_io_context_, m_ip.empty() ? asio::ip::tcp::endpoint(asio::ip::tcp::v4(), atoi(m_port.c_str())) : asio::ip::tcp::endpoint(asio::ip::address::from_string(m_ip.c_str()), atoi(m_port.c_str())));
			}
			catch (std::exception& e)
			{
				std::string error = e.what();
				if (m_log_callback_fun != nullptr)
				{
					m_log_callback_fun(error);
				}
				return false;
			};

			_async_accept();
			__check_client();
			__timer();

			m_work = std::make_shared<asio::io_service::work>(asio::io_service::work(m_io_context_));

			while (m_threads.size() < 4)
			{
				m_threads.emplace_back(std::thread([this]()
					{
						do
						{
							try
							{
								m_io_context_.run();
								break;
							}
							catch (std::exception& e)
							{
								std::string error = e.what();
								if (m_log_callback_fun != nullptr)
								{
									m_log_callback_fun(error);
								}
							};
						} while (m_appEnable);
					}));
			};

			return true;
		};
	private:
		bool _async_accept() {
			if (!m_appEnable)
			{
				return false;
			}

			std::shared_ptr<asio::ip::tcp::socket> client = std::make_shared<asio::ip::tcp::socket>(m_io_context_);

			if (client == nullptr || m_acceptor_ == nullptr)
			{
				return false;
			}

			m_acceptor_->async_accept(*client, m_strand.wrap([this, client](std::error_code ec) {
				if (ec)
				{
					return false;
				}

				auto id = client->remote_endpoint().address().to_string() + ": " + std::to_string(client->remote_endpoint().port());

				if (m_accept_client_callback_fun != nullptr)
				{
					if (!m_accept_client_callback_fun(client))
					{
						if (m_close_client_callback_fun != nullptr)
						{
							m_close_client_callback_fun(client, "accept client fail.");
						}
						client->close();
					}
				}

				if (client->is_open())
				{
					std::unique_lock<decltype(m_client_data_mutex)> lock(m_client_data_mutex);
					if (m_client_data_map.find(client) == m_client_data_map.end())
					{
						m_client_data_map[client] = std::shared_ptr<client_data>(new client_data(id));
					}
					else
					{
						m_client_data_map[client]->m_id = id;
						while (!m_client_data_map[client]->m_send_queue.empty())
						{
							m_client_data_map[client]->m_send_queue.pop();
						}
						m_client_data_map[client]->m_buf.fill('\0');
					}
					m_client_data_map[client]->m_connect_time_point = std::chrono::system_clock::now();
					lock.unlock();

					_async_receive(client, m_client_data_map[client]);
				}

				if (m_appEnable)
				{
					_async_accept();
				}
				return true;
				}));

			return true;
		};
		bool _async_send(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data, const char* buf, const int& size) {
			if (!m_appEnable)
			{
				return false;
			}

			if (buf == nullptr || client == nullptr)
			{
				return false;
			}

			if (!client->is_open())
			{
				return false;
			}

			std::shared_ptr<std::vector<char>> buf_ = std::make_shared<std::vector<char>>(buf, buf + size);

			if (buf_ == nullptr)
			{
				return false;
			}

			asio::post(m_io_context_, m_strand.wrap([this, client, data, buf_]()
				{
					data->m_send_queue.emplace(buf_);
					__queue_send(client, data);

					return true;

				}));

			return true;
		};
		bool _async_receive(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data) {
			if (!m_appEnable)
			{
				return false;
			}

			if (client == nullptr || data == nullptr)
			{
				return false;
			}

			if (!client->is_open())
			{
				return false;
			}

			data->m_buf.fill('\0');

			asio::async_read(*client, asio::buffer(data->m_buf.data(), sizeof(protocol_head)), m_strand.wrap([this, client, data](std::error_code ec, std::size_t length)
				{
					if (ec)
					{
						if (m_close_client_callback_fun != nullptr)
						{
							m_close_client_callback_fun(client, "async_read fail. ec: " + ec.message());
						}
						client->close();
						return false;
					};

					int size = ((const protocol_head*)data->m_buf.data())->size;
					if (size > data->m_buf.size() || sizeof(protocol_head) > size)
					{
						if (m_close_client_callback_fun != nullptr)
						{
							m_close_client_callback_fun(client, "async_read size error.");
						}
						client->close();
						return false;
					}

					asio::async_read(*client, asio::buffer(data->m_buf.data() + sizeof(protocol_head), size - sizeof(protocol_head)), m_strand.wrap([this, client, data, size](std::error_code ec, std::size_t length)
						{
							if (ec)
							{
								if (m_close_client_callback_fun != nullptr)
								{
									m_close_client_callback_fun(client, "async_read fail. ec: " + ec.message());
								}
								client->close();
								return false;
							};

							if (length != size - sizeof(protocol_head))
							{
								if (m_close_client_callback_fun != nullptr)
								{
									m_close_client_callback_fun(client, "async_read size error.");
								}
								client->close();
								return false;
							}

							data->m_receive_count += size;
							data->m_receive_time_point = std::chrono::system_clock::now();

							asio::post(m_io_context_, [this, client, data, length]() {
								// 处理数据
								if (m_deal_buf_callback_fun != nullptr)
								{
									if (!m_deal_buf_callback_fun(std::bind(&CTcpServer::_async_send, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4), client, data, data->m_buf.data() + sizeof(protocol_head), length))
									{
										if (m_close_client_callback_fun != nullptr)
										{
											m_close_client_callback_fun(client, "deal buf fail.");
										}
										client->close();
										return false;
									}
								};

								_async_receive(client, data);
								return true;
								});


							return true;

						}));

					return true;
				}));

			return true;
		};
	private:
		bool __queue_send(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data) {
			if (!m_appEnable)
			{
				return false;
			}

			if (client == nullptr || data == nullptr)
			{
				return false;
			}

			if (!client->is_open())
			{
				return false;
			}

			if (data->m_send_queue.empty())
			{
				return false;
			}

			auto buf = data->m_send_queue.front();
			data->m_send_queue.pop();

			asio::async_write(*client, asio::buffer(buf->data(), buf->size()), m_strand.wrap([this, client, data](asio::error_code ec, std::size_t len) {
				if (ec)
				{
					if (m_close_client_callback_fun != nullptr)
					{
						m_close_client_callback_fun(client, "async write fail. ec: " + ec.message());
					}
					client->close();
					return false;

				}
				else
				{
					data->m_send_count += len;
					data->m_send_time_point = std::chrono::system_clock::now();

					__queue_send(client, data);
					return true;
				}
				}));

			return true;
		};
		bool __check_client() {
			if (!m_appEnable)
			{
				return false;
			}

			std::unique_lock<decltype(m_client_data_mutex)> lock(m_client_data_mutex);
			for (auto it = m_client_data_map.begin(); it != m_client_data_map.end();)
			{
				if ((*it).first != nullptr)
				{
					if (!(*it).first->is_open())
					{
						m_client_data_map.erase(it++);
						continue;
					}
				}
				it++;
			}

			m_check_client_timer.expires_after(std::chrono::milliseconds(100));
			m_check_client_timer.async_wait(m_strand.wrap([this](std::error_code ec) {
				if (ec)
				{
					return false;
				}
				__check_client();
				return true;
				}));
			return true;
		};
		bool __delete_client() {
			std::unique_lock<decltype(m_client_data_mutex)> lock(m_client_data_mutex);
			for (auto it = m_client_data_map.begin(); it != m_client_data_map.end();)
			{
				if ((*it).first != nullptr)
				{
					if ((*it).first->is_open())
					{
						(*it).first->close();
					}
				}
				m_client_data_map.erase(it++);
			}
			return true;
		};
		bool __timer() {
			if (!m_appEnable)
			{
				return false;
			}

			static std::chrono::milliseconds milliseconds(100);
			if (m_timer_callback_fun != nullptr)
			{
				if (!m_timer_callback_fun(milliseconds, std::make_shared<std::map<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<client_data>>>(m_client_data_map)))
				{
					return false;
				}
			}

			m_timer.expires_after(milliseconds);
			m_timer.async_wait(m_strand.wrap([this](std::error_code ec) {
				if (ec)
				{
					return false;
				}
				__timer();
				return true;
				}));
			return true;
		};
	private:
		std::vector<std::thread> m_threads;
		std::atomic<bool> m_appEnable;

		asio::io_context m_io_context_;
		asio::io_context::strand m_strand;
		std::shared_ptr<asio::io_service::work> m_work;
		std::shared_ptr<asio::ip::tcp::acceptor> m_acceptor_;
		asio::steady_timer m_check_client_timer;
		asio::steady_timer m_timer;

		std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client)> m_accept_client_callback_fun;

		std::function<bool(std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data, const char* buf, const int& size)> async_send_fun,
			std::shared_ptr<asio::ip::tcp::socket> client,
			std::shared_ptr<client_data> data,
			const char* buf,
			const int& size)> m_deal_buf_callback_fun;

		std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, const std::string& error)> m_close_client_callback_fun;

		std::function<bool(std::chrono::milliseconds& milliseconds, std::shared_ptr<std::map<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<client_data>>>)> m_timer_callback_fun;

		std::function<bool(const std::string& msg)> m_log_callback_fun;

	private:
		std::string m_ip;
		std::string m_port;

		std::mutex m_client_data_mutex;
		std::map<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<client_data>> m_client_data_map;
	};




	class CTcpClient
	{
	public:
		CTcpClient() = delete;
		CTcpClient(const std::string& ip = "",
			const std::string& port = "8000",
			std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client)> connect_server_callback_fun = nullptr,
			std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, const std::string& error)> close_client_callback_fun = nullptr,
			std::function<bool(std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data, const char* buf, const int& size)> async_send_fun, std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data, const char* buf, const int& size)> handle_buf_callback_fun = nullptr,
			std::function<bool(std::chrono::milliseconds& milliseconds, std::shared_ptr<std::pair<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<client_data>>>)> timer_callback_fun = nullptr,
			std::function<bool(const std::string& msg)> log_callback_fun = nullptr)
			: m_ip(ip)
			, m_port(port)
			, m_appEnable(true)
			, m_strand(m_io_context_)
			, m_connect_server_callback_fun(connect_server_callback_fun)
			, m_close_client_callback_fun(close_client_callback_fun)
			, m_deal_buf_callback_fun(handle_buf_callback_fun)
			, m_timer_callback_fun(timer_callback_fun)
			, m_log_callback_fun(log_callback_fun)
			, m_check_client_timer(m_io_context_)
			, m_timer(m_io_context_)
		{
		}

		~CTcpClient() {
			m_io_context_.post([this]()
				{
					m_appEnable = false;

					m_work.reset();
				});

			for (auto& t : m_threads)
			{
				t.join();
			}
		};
	public:
		bool run() {
			_async_connect();
			__timer();

			m_work = std::make_shared<asio::io_service::work>(asio::io_service::work(m_io_context_));

			while (m_threads.size() < 1)
			{
				m_threads.emplace_back(std::thread([this]()
					{
						do
						{
							try
							{
								m_io_context_.run();
								break;
							}
							catch (std::exception& e)
							{
								std::string error = e.what();
								if (m_log_callback_fun != nullptr)
								{
									m_log_callback_fun(error);
								}
							};
						} while (m_appEnable);
					}));
			};

			return true;
		};
	public:
		bool AsyncSend(const char* buf, const int& size) {
			return _async_send(m_client_data_pair.first, m_client_data_pair.second, buf, size);
		};
	private:
		bool _async_connect() {
			std::shared_ptr<asio::ip::tcp::socket> client = std::make_shared<asio::ip::tcp::socket>(m_io_context_);
			client->async_connect(asio::ip::tcp::endpoint(asio::ip::address::from_string(m_ip.c_str()), atoi(m_port.c_str())), m_strand.wrap([this, client](asio::error_code ec) {
				if (ec)
				{
					std::this_thread::sleep_for(std::chrono::seconds(1));
					if (m_log_callback_fun != nullptr)
					{
						m_log_callback_fun(ec.message());
					}
					_async_connect();
					return false;
				}

				if (m_connect_server_callback_fun != nullptr)
				{
					if (!m_connect_server_callback_fun(client))
					{
						if (m_close_client_callback_fun != nullptr)
						{
							m_close_client_callback_fun(client, "connect server fail.");
						}
						client->close();
					}
				}

				if (client->is_open())
				{
					std::string id;
					try
					{
						id = client->remote_endpoint().address().to_string() + ":" + std::to_string(client->remote_endpoint().port());
					}
					catch (std::exception& e)
					{
						return false;
					}

					std::unique_lock<decltype(m_client_data_mutex)> lock(m_client_data_mutex);
					m_client_data_pair = std::make_pair(client, std::shared_ptr<client_data>(new client_data(id)));
					m_client_data_pair.second->m_connect_time_point = std::chrono::system_clock::now();
					lock.unlock();

					_async_receive(client, m_client_data_pair.second);
				}
				return true;
				}));
			return true;
		};
		bool _async_send(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data, const char* buf, const int& size) {
			if (!m_appEnable)
			{
				return false;
			}

			if (buf == nullptr || client == nullptr)
			{
				return false;
			}

			if (!client->is_open())
			{
				return false;
			}

			std::shared_ptr<std::vector<char>> buf_ = std::make_shared<std::vector<char>>(buf, buf + size);

			if (buf_ == nullptr)
			{
				return false;
			}

			asio::post(m_io_context_, m_strand.wrap([this, client, data, buf_]()
				{
					data->m_send_queue.emplace(buf_);
					__queue_send(client, data);

					return true;

				}));

			return true;
		};
		bool _async_receive(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data) {
			if (!m_appEnable)
			{
				return false;
			}

			if (client == nullptr || data == nullptr)
			{
				return false;
			}

			if (!client->is_open())
			{
				return false;
			}

			data->m_buf.fill('\0');

			asio::async_read(*client, asio::buffer(data->m_buf.data(), sizeof(protocol_head)), m_strand.wrap([this, client, data](std::error_code ec, std::size_t length)
				{
					if (ec)
					{
						if (m_close_client_callback_fun != nullptr)
						{
							m_close_client_callback_fun(client, "async_read fail. ec: " + ec.message());
						}
						client->close();

						std::this_thread::sleep_for(std::chrono::seconds(1));
						_async_connect();
						return false;
					};

					int size = ((const protocol_head*)data->m_buf.data())->size;
					if (size > data->m_buf.size() || sizeof(protocol_head) > size)
					{
						if (m_close_client_callback_fun != nullptr)
						{
							m_close_client_callback_fun(client, "async_read size error.");
						}
						client->close();

						std::this_thread::sleep_for(std::chrono::seconds(1));
						_async_connect();
						return false;
					}

					asio::async_read(*client, asio::buffer(data->m_buf.data() + sizeof(protocol_head), size - sizeof(protocol_head)), m_strand.wrap([this, client, data, size](std::error_code ec, std::size_t length)
						{
							if (ec)
							{
								if (m_close_client_callback_fun != nullptr)
								{
									m_close_client_callback_fun(client, "async_read fail. ec: " + ec.message());
								}
								client->close();

								std::this_thread::sleep_for(std::chrono::seconds(1));
								_async_connect();
								return false;
							};

							if (length != size - sizeof(protocol_head))
							{
								if (m_close_client_callback_fun != nullptr)
								{
									m_close_client_callback_fun(client, "async_read size error.");
								}
								client->close();

								std::this_thread::sleep_for(std::chrono::seconds(1));
								_async_connect();
								return false;
							}

							data->m_receive_count += size;
							data->m_receive_time_point = std::chrono::system_clock::now();

							asio::post(m_io_context_, [this, client, data, length]() {
								// 处理数据
								if (m_deal_buf_callback_fun != nullptr)
								{
									if (!m_deal_buf_callback_fun(std::bind(&CTcpClient::_async_send, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4), client, data, data->m_buf.data() + sizeof(protocol_head), length))
									{
										if (m_close_client_callback_fun != nullptr)
										{
											m_close_client_callback_fun(client, "deal buf fail.");
										}
										client->close();

										std::this_thread::sleep_for(std::chrono::seconds(1));
										_async_connect();
										return false;
									}
								};

								_async_receive(client, data);
								return true;
								});
							return true;
						}));
					return true;
				}));
			return true;
		};
	private:
		bool __queue_send(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data) {
			if (!m_appEnable)
			{
				return false;
			}

			if (client == nullptr || data == nullptr)
			{
				return false;
			}

			if (!client->is_open())
			{
				return false;
			}

			if (data->m_send_queue.empty())
			{
				return false;
			}

			auto buf = data->m_send_queue.front();
			data->m_send_queue.pop();

			asio::async_write(*client, asio::buffer(buf->data(), buf->size()), m_strand.wrap([this, client, data](asio::error_code ec, std::size_t len) {
				if (ec)
				{
					if (m_close_client_callback_fun != nullptr)
					{
						m_close_client_callback_fun(client, "async write fail. ec: " + ec.message());
					}
					client->close();

					std::this_thread::sleep_for(std::chrono::seconds(1));
					_async_connect();
					return false;

				}
				else
				{
					data->m_send_count += len;
					data->m_send_time_point = std::chrono::system_clock::now();

					__queue_send(client, data);
					return true;
				}
				}));

			return true;
		};
		bool __timer() {
			if (!m_appEnable)
			{
				return false;
			}

			static std::chrono::milliseconds milliseconds(100);
			if (m_timer_callback_fun != nullptr)
			{
				if (!m_timer_callback_fun(milliseconds, std::make_shared<std::pair<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<client_data>>>(m_client_data_pair)))
				{
					return false;
				}
			}

			m_timer.expires_after(milliseconds);
			m_timer.async_wait(m_strand.wrap([this](std::error_code ec) {
				if (ec)
				{
					return false;
				}
				__timer();
				return true;
				}));
			return true;
		};
	private:
		std::vector<std::thread> m_threads;
		std::atomic<bool> m_appEnable;

		asio::io_context m_io_context_;
		asio::io_context::strand m_strand;
		std::shared_ptr<asio::io_service::work> m_work;
		asio::steady_timer m_check_client_timer;
		asio::steady_timer m_timer;

		std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client)> m_connect_server_callback_fun;

		std::function<bool(std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<client_data> data, const char* buf, const int& size)> async_send_fun,
			std::shared_ptr<asio::ip::tcp::socket> client,
			std::shared_ptr<client_data> data,
			const char* buf,
			const int& size)> m_deal_buf_callback_fun;

		std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, const std::string& error)> m_close_client_callback_fun;

		std::function<bool(std::chrono::milliseconds& milliseconds, std::shared_ptr<std::pair<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<client_data>>>)> m_timer_callback_fun;

		std::function<bool(const std::string& msg)> m_log_callback_fun;

	private:
		std::string m_ip;
		std::string m_port;

		std::mutex m_client_data_mutex;
		std::pair<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<client_data>> m_client_data_pair;
	};
}