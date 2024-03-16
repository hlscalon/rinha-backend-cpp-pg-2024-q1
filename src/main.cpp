#include <iostream>
#include <thread>
#include <chrono>

#include "libs/httplib.h"
#include "libs/json.hpp"

using json = nlohmann::json;

httplib::Response respond(httplib::Response& res, int status = httplib::StatusCode::OK_200, json responseBody = nullptr) {
	res.status = status;

	if (responseBody != nullptr) {
		res.set_content(responseBody.dump(), "application/json");
	}

	return res;
}

bool isValidClienteId(int id) {
	return id > 0 && id < 6;
}

httplib::Response handleExtrato(int clienteId, httplib::Response& res) {
	res.status = httplib::StatusCode::OK_200;

	return res;
}

httplib::Response handleTransacao(int clienteId, std::string body, httplib::Response& res) {
	json data = json::parse(body);

	if (!data["valor"].is_number_unsigned()) {
		return respond(res, httplib::StatusCode::UnprocessableContent_422);
	}

	std::string tipo = data["tipo"].template get<std::string>();
	if (tipo != "c" && tipo != "d") {
		return respond(res, httplib::StatusCode::UnprocessableContent_422);
	}

	std::string descricao = data["descricao"].template get<std::string>();
	if (descricao == "") {
		return respond(res, httplib::StatusCode::UnprocessableContent_422);
	}

	int descricaoLen = descricao.length();
	if (descricaoLen < 1 || descricaoLen > 10) {
		return respond(res, httplib::StatusCode::UnprocessableContent_422);
	}

	json response;
	response["henrique"] = "henrique";
	return respond(res, httplib::StatusCode::OK_200, response);
}

int main() {
	httplib::Server server;

	server.Get("/clientes/:id/extrato", [&](const httplib::Request& req, httplib::Response& res) {
		auto clienteId = std::stoi(req.path_params.at("id"));
		if (!isValidClienteId(clienteId)) {
			return respond(res, httplib::StatusCode::UnprocessableContent_422);
		}

		return handleExtrato(clienteId, res);
	});

	server.Post("/clientes/:id/transacoes", [&](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader &content_reader) {
		auto clienteId = std::stoi(req.path_params.at("id"));
		if (!isValidClienteId(clienteId)) {
			return respond(res, httplib::StatusCode::UnprocessableContent_422);
		}

		std::string body;
		content_reader([&](const char *data, size_t data_length) {
			body.append(data, data_length);
			return true;
		});

		return handleTransacao(clienteId, body, res);
	});

	std::cout << "Servidor rodando em 0.0.0.0:8080" << std::endl;

	server.listen("0.0.0.0", 8080);
}