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
#include <functional>

#include "../3rd/asio-1.16.0/include/asio.hpp"

namespace posix { namespace tcp
{
	constexpr int max_receive_size = 1024 * 1;

	typedef struct _protocol_head
	{
		int size;
	}protocol_head;

	class ClientData :public std::enable_shared_from_this<ClientData>
	{
	public:
		ClientData(const std::string& id)
			:id_(id)
			, receive_count_(0) 
			, receive_count_last_(0)
			, send_count_(0)
			, send_count_last_(0)
		{
		};
		~ClientData() {};
	public:
		std::string id_;
		std::vector<char> buf_;
		std::queue <std::shared_ptr<std::vector<char>>> send_queue_;
		std::chrono::system_clock::time_point connect_time_point_;
		std::chrono::system_clock::time_point receive_time_point_;
		std::chrono::system_clock::time_point send_time_point_;
		std::size_t receive_count_;
		std::size_t receive_count_last_;
		std::size_t send_count_;
		std::size_t send_count_last_;
	};

	std::shared_ptr<std::vector<char>> PackData(const char* buf, const int& size)
	{
		auto ret = std::make_shared<std::vector<char>>();
		if (!ret) { return nullptr; }
		ret->resize(sizeof(protocol_head) + size);
		auto head = reinterpret_cast<protocol_head*>(ret->data());
		head->size = sizeof(protocol_head) + size;
		std::memcpy(ret->data() + sizeof(protocol_head), buf, size);
		return std::move(ret);
	}

	std::string GetId(std::shared_ptr<asio::ip::tcp::socket> s)
	{
		if (s) { return  s->remote_endpoint().address().to_string() + ":" + std::to_string(s->remote_endpoint().port()); }	
		else { return std::string(); }
	}

	class TcpServer
	{
	public:
		using on_log_type = std::function<bool(const std::string& msg)>;
		using on_accept_type = std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client)>;
		using on_send_type = std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size)>;
		using on_pre_receive_type = std::function<int(std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size)>;
		using on_receive_type = std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size, on_send_type on_send)>;
		using client_data_map_type = std::map<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<ClientData>>;
		using on_timer_type = std::function<bool(std::chrono::milliseconds& milliseconds, client_data_map_type&)>;
		
	public:
		TcpServer() = delete;
		TcpServer(
			const std::string& ip = "0.0.0.0",
			const std::string& port = "8000",
			on_receive_type on_receive = nullptr,
			on_log_type on_log = nullptr,
			on_accept_type on_accept = nullptr,
			on_timer_type on_timer = nullptr,
			on_pre_receive_type on_pre_receive = [](std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size) { 
				auto receive_length = ((const protocol_head*)data)->size;
				if (receive_length > max_receive_size || receive_length < sizeof(protocol_head)) { return -1; }
				return receive_length;
			}
			)
			: ip_(ip)
			, port_(port)
			, running_(true)
			, strand_(io_context_)
			, check_client_timer_(io_context_)
			, timer_(io_context_)
			, on_receive_(on_receive)
			, on_accept_(on_accept)
			, on_log_(on_log)
			, on_timer_(on_timer)


		{
		};

		~TcpServer() {
			running_ = false;
			work_.reset();
			acceptor_->close();
			io_context_.stop();
			while (!io_context_.stopped()) { std::this_thread::sleep_for(std::chrono::seconds(1)); };
			DeleteClient();
			for (auto& t : threads_)
			{
				t.join();
			}
		};
	public:
		bool Run() {
			try
			{
				acceptor_ = std::make_shared<asio::ip::tcp::acceptor>(io_context_, ip_.empty() ? asio::ip::tcp::endpoint(asio::ip::tcp::v4(), atoi(port_.c_str())) : asio::ip::tcp::endpoint(asio::ip::address::from_string(ip_.c_str()), atoi(port_.c_str())));
			}
			catch (std::exception& e)
			{
				std::string error = e.what();
				if (on_log_ != nullptr) { on_log_("run error: " + error); }
				return false;
			};

			AsyncAccept();
			CheckClient();
			DoTimer();

			work_ = std::make_shared<asio::io_service::work>(asio::io_service::work(io_context_));

			while (threads_.size() < 4)
			{
				threads_.emplace_back(std::thread([this]()
					{
						do
						{
							try
							{
								io_context_.run();
								break;
							}
							catch (std::exception& e)
							{
								std::string error = e.what();
								if (on_log_ != nullptr) { on_log_("run error: " + error); }
							};
						} while (running_);
					}));
			};
			if (on_log_ != nullptr) { on_log_("server run ..."); };
			return true;
		};
	private:
		bool AsyncAccept() {
			if (!running_) { return false; }

			std::shared_ptr<asio::ip::tcp::socket> client = std::make_shared<asio::ip::tcp::socket>(io_context_);

			if (client == nullptr || acceptor_ == nullptr) { return false; }
			
			acceptor_->async_accept(*client, strand_.wrap([this, client](std::error_code ec) {
				if (ec)
				{
					if (on_log_ != nullptr) { on_log_("asyncAccept error: " + ec.message() + " code: " + std::to_string(ec.value())); }
					return false;
				}

				if (on_accept_ != nullptr) 
				{ 
					if (!on_accept_(client) && running_) 
					{ 
						AsyncAccept();
						return false; 
					};
				};

				if (client->is_open())
				{
					auto id = GetId(client);

					if (on_log_ != nullptr) { on_log_("server accept: " + id); };

					if (client_data_map_.find(client) == client_data_map_.end())
					{
						client_data_map_[client] = std::make_shared<ClientData>(id);
					}
					else
					{
						client_data_map_[client]->id_ = id;
						while (!client_data_map_[client]->send_queue_.empty()) { client_data_map_[client]->send_queue_.pop(); }
					}
					client_data_map_[client]->connect_time_point_ = std::chrono::system_clock::now();
					AsyncReceive(client);						
				}

				if (running_) { AsyncAccept(); }
				return true;
				}));
			return true;
		};

		bool AsyncSend(std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size) {
			if (!running_ || data == nullptr || client == nullptr || !client->is_open()) { return false; }

			std::shared_ptr<std::vector<char>> buf_ = std::make_shared<std::vector<char>>(data, data + size);
			if (buf_ == nullptr) { return false; }
			auto& client_data = client_data_map_[client];
			asio::post(io_context_, strand_.wrap([this, client, client_data, buf_]()
				{
					client_data->send_queue_.emplace(buf_);
					QueueSend(client);
					return true;

				}));
			return true;
		};

		bool AsyncReceive(std::shared_ptr<asio::ip::tcp::socket> client) {
			if (!running_ || client == nullptr || !client->is_open()) { return false; }

			auto& client_data = client_data_map_[client];

			if (on_pre_receive_)
			{
				client_data->buf_.resize(sizeof(protocol_head));
				asio::async_read(*client, asio::buffer(client_data->buf_.data(), client_data->buf_.size()), strand_.wrap([this, client, client_data](std::error_code ec, std::size_t length)
					{
						if (ec)
						{
							if (on_log_ != nullptr) { on_log_("async_read fail. close client: " + GetId(client) + " ec: " + ec.message()); }
							client->close();
							return false;
						};

						auto receive_length = on_pre_receive_(client, client_data->buf_.data(), client_data->buf_.size());
						if (receive_length > 0)
						{
							client_data->buf_.resize(receive_length);
							asio::async_read(*client, asio::buffer(client_data->buf_.data() + sizeof(protocol_head), receive_length - sizeof(protocol_head)), strand_.wrap([this, client, client_data, receive_length](std::error_code ec, std::size_t length)
								{
									if (ec)
									{
										if (on_log_ != nullptr) { on_log_("async read fail. ec: " + ec.message()); }
										client->close();
										return false;
									};

									if (length != receive_length - sizeof(protocol_head))
									{
										if (on_log_ != nullptr) { on_log_("async read size error: " + std::to_string(length)); }
										client->close();
										return false;
									}

									client_data->receive_count_ += client_data->buf_.size();
									client_data->receive_time_point_ = std::chrono::system_clock::now();

									asio::post(io_context_, strand_.wrap([this, client, client_data]() {
										// 处理数据
										if (on_receive_ != nullptr)
										{
											if (!on_receive_(client, client_data->buf_.data(), client_data->buf_.size(), std::bind(&TcpServer::AsyncSend, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)))
											{
												if (on_log_ != nullptr) { on_log_("close client: " + GetId(client) + ",deal buf fail."); }
												client->close();
												return false;
											}
										};

										AsyncReceive(client);
										return true;
										}));
									return true;
								}));
						};
						return true;
					}));
			}
			else
			{
				if (on_log_ != nullptr) { on_log_("default receive size: " + std::to_string(max_receive_size)); };

				client_data->buf_.resize(max_receive_size);
				asio::async_read(*client, asio::buffer(client_data->buf_.data(), client_data->buf_.size()), strand_.wrap([this, client, client_data](std::error_code ec, std::size_t length)
					{
						if (ec)
						{
							if (on_log_ != nullptr) { on_log_("async_read fail. close client: " + GetId(client) + " ec: " + ec.message()); }
							client->close();
							return false;
						};

						client_data->receive_count_ += client_data->buf_.size();
						client_data->receive_time_point_ = std::chrono::system_clock::now();

						asio::post(io_context_, strand_.wrap([this, client, client_data]() {
							// 处理数据
							if (on_receive_ != nullptr)
							{
								if (!on_receive_(client, client_data->buf_.data(), client_data->buf_.size(), std::bind(&TcpServer::AsyncSend, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)))
								{
									if (on_log_ != nullptr) { on_log_("close client: " + GetId(client) + ",deal buf fail."); }
									client->close();
									return false;
								}
							};

							if (on_log_ != nullptr) { on_log_("server read len: " + std::to_string(client_data->buf_.size())); };

							AsyncReceive(client);
							return true;
							}));
						return true;
					}));
			}
			return true;
		};
	private:
		bool QueueSend(std::shared_ptr<asio::ip::tcp::socket> client) {
			if (!running_ || client == nullptr || !client->is_open()) { return false; }

			auto& client_data = client_data_map_[client];
			if(client_data->send_queue_.empty()) { return false; }

			asio::async_write(*client, asio::buffer(client_data->send_queue_.front()->data(), client_data->send_queue_.front()->size()), strand_.wrap([this, client, client_data](asio::error_code ec, std::size_t len) {
				if (ec)
				{
					if (on_log_ != nullptr) { on_log_("client: " + GetId(client) + " async write fail. ec: " + ec.message()); }
					client->close();
					return false;
				}

				client_data->send_count_ += len;
				client_data->send_time_point_ = std::chrono::system_clock::now();
				client_data->send_queue_.pop();
				QueueSend(client);
				return true;
				}));
			return true;
		};
		bool CheckClient() {
			if (!running_) { return false; }

			check_client_timer_.expires_after(std::chrono::milliseconds(10));
			check_client_timer_.async_wait(strand_.wrap([this](std::error_code ec) {
				if (ec) { return false; }

				for (auto it = client_data_map_.begin(); it != client_data_map_.end();)
				{
					if ((*it).first != nullptr)
					{
						if (!(*it).first->is_open())
						{
							client_data_map_.erase(it++);
							continue;
						}
					}
					it++;
				}
				CheckClient();
				return true;
				}));
			return true;
		};
		bool DeleteClient() {
			for (auto it = client_data_map_.begin(); it != client_data_map_.end();)
			{
				if ((*it).first != nullptr)
				{
					if ((*it).first->is_open())
					{
						(*it).first->close();
					}
				}
				client_data_map_.erase(it++);
			}
			return true;
		};

		bool DoTimer() {
			if (!running_) { return false; }

			static std::chrono::milliseconds milliseconds(100);
			timer_.expires_after(milliseconds);
			timer_.async_wait(strand_.wrap([&, this](std::error_code ec) {
				if (ec) { return false; }
				if (on_timer_ != nullptr)
				{
					if (!on_timer_(milliseconds, client_data_map_))
					{
						return false;
					}
				}

				DoTimer();
				return true;
				}));
			return true;
		};
	private:
		std::vector<std::thread> threads_;
		std::atomic<bool> running_;

		asio::io_context io_context_;
		asio::io_context::strand strand_;
		std::shared_ptr<asio::io_service::work> work_;
		std::shared_ptr<asio::ip::tcp::acceptor> acceptor_;
		asio::steady_timer check_client_timer_;
		asio::steady_timer timer_;

		on_accept_type on_accept_;
		on_receive_type on_receive_;
		on_log_type on_log_;
		on_timer_type on_timer_;
		on_pre_receive_type on_pre_receive_;

	private:
		std::string ip_;
		std::string port_;

		std::map<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<ClientData>> client_data_map_;
	};

	class TcpClient
	{
	public:
		using on_log_type = std::function<bool(const std::string& msg)>;
		using on_connect_type = std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client)>;
		using on_send_type = std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size)>;
		using on_pre_receive_type = std::function<int(std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size)>;
		using on_receive_type = std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size, on_send_type on_send)>;
		using client_data_map_type = std::map<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<ClientData>>;
		using on_timer_type = std::function<bool(std::chrono::milliseconds& milliseconds, client_data_map_type&)>;

	public:
		TcpClient() = delete;
		TcpClient(
			const std::string& ip = "127.0.0.1",
			const std::string& port = "8000",
			on_receive_type on_receive = nullptr,
			on_connect_type on_connect = nullptr,
			on_log_type on_log = nullptr,
			on_timer_type on_timer = nullptr,
			on_pre_receive_type on_pre_receive = [](std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size) {
				auto receive_length = ((const protocol_head*)data)->size;
				if (receive_length > max_receive_size || receive_length < sizeof(protocol_head)) { return -1; }
				return receive_length;
			}
		)
			: ip_(ip)
			, port_(port)
			, running_(true)
			, strand_(io_context_)
			, check_client_timer_(io_context_)
			, timer_(io_context_)
			, on_connect_(on_connect)
			, on_receive_(on_receive)
			, on_timer_(on_timer)
			, on_log_(on_log)

		{
		}

		~TcpClient() {
			running_ = false;
			work_.reset();
			io_context_.stop();
			while (!io_context_.stopped()) { std::this_thread::sleep_for(std::chrono::seconds(1)); }
			for (auto& t : threads_)
			{
				t.join();
			}
		};
	public:
		bool Run() 
		{
			AsyncConnect();
			DoTimer();

			work_ = std::make_shared<asio::io_service::work>(asio::io_service::work(io_context_));

			while (threads_.size() < 1)
			{
				threads_.emplace_back(std::thread([this]()
					{
						do
						{
							try
							{
								io_context_.run();
								break;
							}
							catch (std::exception& e)
							{
								std::string error = e.what();
								if (on_log_ != nullptr) { on_log_(error); }
							};
						} while (running_);
					}));
			};
			if (on_log_ != nullptr) { on_log_("client run ..."); };
			return true;
		};
		bool Send(const char* data, const int& size)
		{
			if (data == nullptr) { return false; };

			for (auto& it : client_data_map_)
			{
				if (it.first->is_open()) AsyncSend(it.first, data, size);
			}
	
			return true;
		}
	private:
		bool AsyncConnect() {
			std::shared_ptr<asio::ip::tcp::socket> client = std::make_shared<asio::ip::tcp::socket>(io_context_);
			client->async_connect(asio::ip::tcp::endpoint(asio::ip::address::from_string(ip_.c_str()), atoi(port_.c_str())), strand_.wrap([this, client](asio::error_code ec) {
				if (ec)
				{
					if (on_log_ != nullptr) { on_log_("connect fail: " + ec.message()); }
					return false;
				}

				if (on_connect_ != nullptr)
				{
					if (!on_connect_(client) && running_)
					{
						AsyncConnect();
						return false;
					};
				};

				if (client->is_open())
				{
					std::string id = GetId(client);

					if (on_log_ != nullptr) { on_log_("client connect: " + id); };

					if (client_data_map_.find(client) == client_data_map_.end())
					{
						client_data_map_[client] = std::make_shared<ClientData>(id);
					}
					
					client_data_map_[client]->connect_time_point_ = std::chrono::system_clock::now();
					AsyncReceive(client);
				}
				return true;
				}));
			return true;
		};

		bool AsyncSend(std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size) {
			if (!running_ || data == nullptr || client == nullptr || !client->is_open()) { return false; }

			std::shared_ptr<std::vector<char>> buf_ = std::make_shared<std::vector<char>>(data, data + size);
			if (buf_ == nullptr) { return false; }
			auto& client_data = client_data_map_[client];
			asio::post(io_context_, strand_.wrap([this, client, client_data, buf_]()
				{
					client_data->send_queue_.emplace(buf_);
					QueueSend(client);
					return true;

				}));
			return true;
		};

		bool AsyncReceive(std::shared_ptr<asio::ip::tcp::socket> client) {
			if (!running_ || client == nullptr || !client->is_open()) { return false; }

			auto& client_data = client_data_map_[client];

			if (on_pre_receive_)
			{
				client_data->buf_.resize(sizeof(protocol_head));
				asio::async_read(*client, asio::buffer(client_data->buf_.data(), client_data->buf_.size()), strand_.wrap([this, client, client_data](std::error_code ec, std::size_t length)
					{
						if (ec)
						{
							if (on_log_ != nullptr) { on_log_("Async_read fail. close client: " + GetId(client) + " ec: " + ec.message()); }
							client->close();
							return false;
						};

						if (on_pre_receive_)
						{
							auto receive_length = on_pre_receive_(client, client_data->buf_.data(), client_data->buf_.size());
							if (receive_length > 0)
							{
								client_data->buf_.resize(receive_length);
								asio::async_read(*client, asio::buffer(client_data->buf_.data() + sizeof(protocol_head), receive_length - sizeof(protocol_head)), strand_.wrap([this, client, client_data, receive_length](std::error_code ec, std::size_t length)
									{
										if (ec)
										{
											if (on_log_ != nullptr) { on_log_("async_read fail. ec: " + ec.message()); }
											client->close();
											return false;
										};

										if (length != receive_length - sizeof(protocol_head))
										{
											if (on_log_ != nullptr) { on_log_("async_read size error: " + std::to_string(length)); }
											client->close();
											return false;
										}

										client_data->receive_count_ += client_data->buf_.size();
										client_data->receive_time_point_ = std::chrono::system_clock::now();

										asio::post(io_context_, strand_.wrap([this, client, client_data]() {
											// 处理数据
											if (on_receive_ != nullptr)
											{
												if (!on_receive_(client, client_data->buf_.data(), client_data->buf_.size(), std::bind(&TcpClient::AsyncSend, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)))
												{
													if (on_log_ != nullptr) { on_log_("close client: " + GetId(client) + ",deal buf fail."); }
													client->close();
													return false;
												}
											};

											AsyncReceive(client);
											return true;
											}));
										return true;
									}));
							};
						}
						return true;
					}));
			}
			else
			{
				if (on_log_ != nullptr) { on_log_("default receive size: " + std::to_string(max_receive_size)); };

				client_data->buf_.resize(max_receive_size);
				asio::async_read(*client, asio::buffer(client_data->buf_.data(), client_data->buf_.size()), strand_.wrap([this, client, client_data](std::error_code ec, std::size_t length)
					{
						if (ec)
						{
							if (on_log_ != nullptr) { on_log_("async_read fail. close client: " + GetId(client) + " ec: " + ec.message()); }
							client->close();
							return false;
						};

						client_data->receive_count_ += client_data->buf_.size();
						client_data->receive_time_point_ = std::chrono::system_clock::now();

						asio::post(io_context_, strand_.wrap([this, client, client_data]() {
							// 处理数据
							if (on_receive_ != nullptr)
							{
								if (!on_receive_(client, client_data->buf_.data(), client_data->buf_.size(), std::bind(&TcpClient::AsyncSend, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)))
								{
									if (on_log_ != nullptr) { on_log_("close client: " + GetId(client) + ",deal buf fail."); }
									client->close();
									return false;
								}
							};

							if (on_log_ != nullptr) { on_log_("client receive len: " + std::to_string(client_data->buf_.size())); };

							AsyncReceive(client);
							return true;
							}));
						return true;
					}));
			}
			return true;
		};
	private:
		bool QueueSend(std::shared_ptr<asio::ip::tcp::socket> client) {
			if (!running_ || client == nullptr || !client->is_open()) { return false; }

			auto& client_data = client_data_map_[client];
			if (client_data->send_queue_.empty()) { return false; }

			asio::async_write(*client, asio::buffer(client_data->send_queue_.front()->data(), client_data->send_queue_.front()->size()), strand_.wrap([this, client, client_data](asio::error_code ec, std::size_t len) {
				if (ec)
				{
					if (on_log_ != nullptr) { on_log_("client: " + GetId(client) + " async write fail. ec: " + ec.message()); }
					client->close();
					return false;
				}

				client_data->send_count_ += len;
				client_data->send_time_point_ = std::chrono::system_clock::now();
				client_data->send_queue_.pop();
				QueueSend(client);
				return true;
				}));
			return true;
		};

		bool DoTimer() {
			if (!running_) { return false; }

			static std::chrono::milliseconds milliseconds(100);
			timer_.expires_after(milliseconds);
			timer_.async_wait(strand_.wrap([&, this](std::error_code ec) {
				if (ec) { return false; }
				if (on_timer_ != nullptr)
				{
					if (!on_timer_(milliseconds, client_data_map_))
					{
						return false;
					}
				}

				DoTimer();
				return true;
				}));
			return true;
		};
	private:
		std::vector<std::thread> threads_;
		std::atomic<bool> running_;

		asio::io_context io_context_;
		asio::io_context::strand strand_;
		std::shared_ptr<asio::io_service::work> work_;
		asio::steady_timer check_client_timer_;
		asio::steady_timer timer_;

		on_connect_type on_connect_;
		on_receive_type on_receive_;
		on_log_type on_log_;
		on_timer_type on_timer_;
		on_pre_receive_type on_pre_receive_;

	private:
		std::string ip_;
		std::string port_;

		std::map<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<ClientData>> client_data_map_;
	};
}
}