#include <iostream>
#include <thread>

#include "../include/Tcp.hpp"


int main()
{
	auto server_thread = std::thread([]() {
		posix::tcp::TcpServer server(
			"127.0.0.1",
			"8000",
			[](std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size, posix::tcp::TcpServer::on_send_type on_send)->bool {
				std::cout << "[TcpServer]: receive: " << data << std::endl;
				return true;
			},
			[](const std::string& msg){
				std::cout << "[TcpServer]: " << msg << std::endl;
				return true;
			}
			);


		server.Run();

		while (true)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}

		});
		
	posix::tcp::TcpClient client(
		"127.0.0.1",
		"8000",
		[](std::shared_ptr<asio::ip::tcp::socket> client, const char* data, const int& size, posix::tcp::TcpServer::on_send_type on_send)->bool {
			std::cout << "[TcpServer]: receive: " << data << std::endl;
			return true;
		},
		nullptr,
		[](const std::string& msg) {
			std::cout << "[TcpServer]: " << msg << std::endl;
			return true;
		}
		);

	client.Run();

	while (true)
	{
		std::string msg("hello, i am tcp client.");

		//auto buf = posix::tcp::PackData(msg.data(), msg.size());
		client.Send(msg.data(), msg.size());
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	server_thread.join();


}