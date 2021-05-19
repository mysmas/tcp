#include <iostream>
#include <thread>

#include "../include/Tcp.hpp"


int main()
{
	auto server_thread = std::thread([]() {
		Tcp::CTcpServer server(
			"127.0.0.1",
			"7777",
				[](std::shared_ptr<asio::ip::tcp::socket> client)->bool {
				std::cout << "[CTcpServer]: accept client: " << Tcp::GetId(client) << std::endl;
				return true;
			},
				[](std::shared_ptr<asio::ip::tcp::socket> client, const std::string& error)->bool {
				std::cout << "[CTcpServer]: close client: " << Tcp::GetId(client) << ". Error: " << error <<std::endl;
				return true;
			},
				[](std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<Tcp::client_data> data, const char* buf, const int& size)> async_send_fun, std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<Tcp::client_data> data, const char* buf, const int& size)->bool {
				std::cout << "[CTcpServer]: receive: " << std::string(buf, size) << "from: " << Tcp::GetId(client) <<std::endl;

				std::string msg("hello, i am tcp server.");

				auto buf_ = Tcp::PackData(msg.data(), msg.size());
				async_send_fun(client, data, buf_.data(), buf_.size());
				return true;
			},
				[](std::chrono::milliseconds& milliseconds, std::shared_ptr<std::map<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<Tcp::client_data>>>)->bool {
				milliseconds = std::chrono::milliseconds(10000);
				std::cout << "[CTcpServer]: timer callback." << std::endl;
				return true;
			},
				[](const std::string& msg)->bool {
				std::cout << "[CTcpServer]: " << msg << std::endl;
				return true;
			});


		server.run();

		while (true)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}

		});
		
	Tcp::CTcpClient client(
		"127.0.0.1",
		"7777",
			[](std::shared_ptr<asio::ip::tcp::socket> client)->bool {
			std::cout << "[CTcpClient]: connect server, " << Tcp::GetId(client) << "." << std::endl;
			return true;
		},
			[](std::shared_ptr<asio::ip::tcp::socket> client, const std::string& error)->bool {
			std::cout << "[CTcpClient]: close server, " << Tcp::GetId(client) << ". Error: " << error << std::endl;
			return true;
		},
			[](std::function<bool(std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<Tcp::client_data> data, const char* buf, const int& size)> async_send_fun, std::shared_ptr<asio::ip::tcp::socket> client, std::shared_ptr<Tcp::client_data> data, const char* buf, const int& size)->bool {
			std::cout << "[CTcpClient]: receive: " << std::string(buf, size) << "from: " << Tcp::GetId(client) << std::endl;
			return true;
		},
			[](std::chrono::milliseconds& milliseconds, std::shared_ptr<std::pair<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<Tcp::client_data>>>)->bool {
			milliseconds = std::chrono::milliseconds(10000);
			std::cout << "[CTcpClient]: timer callback." << std::endl;
			return true;
		},
			[](const std::string& msg)->bool {
			std::cout << "[CTcpClient]: " << msg << std::endl;
			return true;
		}
		);

	client.run();

	while (true)
	{
		std::string msg("hello, i am tcp client.");

		auto buf = Tcp::PackData(msg.data(), msg.size());
		client.AsyncSend(buf.data(), buf.size());
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	server_thread.join();


}