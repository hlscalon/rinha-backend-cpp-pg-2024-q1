#include <iostream>
#include <thread>
#include <chrono>
#include <pqxx/pqxx>
#include <cstdlib>

#include "libs/httplib.h"
#include "libs/json.hpp"

using json = nlohmann::json;

constexpr std::string_view queryCreditar {
	"WITH cliente_atualizado AS (UPDATE cliente SET saldo = saldo + {} WHERE id = {} RETURNING saldo, limite) "
	"INSERT INTO transacao (cliente_id, valor, tipo, descricao, limite_atual, saldo_atual) "
		"SELECT {}, {}, {}, {}, cliente_atualizado.limite, cliente_atualizado.saldo "
		"FROM cliente_atualizado "
		"RETURNING limite_atual, saldo_atual"
};

constexpr std::string_view queryDebitar {
	"WITH cliente_atualizado AS (UPDATE cliente SET saldo = saldo - {} WHERE id = {} AND saldo - {} >= -ABS(limite) RETURNING saldo, limite) "
	"INSERT INTO transacao (cliente_id, valor, tipo, descricao, limite_atual, saldo_atual) "
		"SELECT {}, {}, {}, {}, cliente_atualizado.limite, cliente_atualizado.saldo "
		"FROM cliente_atualizado "
		"RETURNING limite_atual, saldo_atual"
};

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

const pqxx::connection getConnection() {
	static std::string dsn = std::format(
		"dbname={} user={} password={} hostaddr={} port={}",
		std::getenv("DB_NAME"),
		std::getenv("DB_USER"),
		std::getenv("DB_PASSWORD"),
		std::getenv("DB_HOST"),
		std::getenv("DB_PORT")
	);

	return pqxx::connection{dsn};
}

httplib::Response handleExtrato(int clienteId, httplib::Response& res) {
	try {
		pqxx::connection conn = getConnection();
		pqxx::work txn{conn};
		pqxx::result result = txn.exec(
			std::format(
				"SELECT valor, tipo, descricao, realizada_em, limite_atual, saldo_atual "
				"FROM transacao "
				"WHERE cliente_id = {} "
				"ORDER BY id DESC "
				"LIMIT 11 ", // Deve pegar uma a mais para ignorar a inicial depois, se necessário
				pqxx::to_string(clienteId)
			)
		);

		int nrow = 1;
		int limiteAtual = 0;
		int saldoAtual = 0;

		json ultimasTransacoes;
		for (auto row : result) {
			if (nrow == 11) {
				break;
			}

			if (nrow == 1) {
				limiteAtual = row["limite_atual"].as<int>();
				saldoAtual = row["saldo_atual"].as<int>();
			}

			json transacao;
			transacao["valor"] = row["valor"].as<int>();
			transacao["tipo"] = row["tipo"].as<std::string>();
			transacao["descricao"] = row["descricao"].as<std::string>();
			transacao["realizada_em"] = row["realizada_em"].as<std::string>();

			ultimasTransacoes.push_back(transacao);

			nrow++;
		}

		txn.commit();

		json responseBody = {
			{
				"saldo", {
					{"total", saldoAtual},
					{"limite", limiteAtual},
					{"data_extrato", "2024-03-16"}  // #TODO: get data atual
				}
			},
			{
				"ultimas_transacoes", ultimasTransacoes
			}
		};

		return respond(res, httplib::StatusCode::OK_200, responseBody);
	} catch (pqxx::sql_error const &e) {
		return respond(res, httplib::StatusCode::UnprocessableContent_422);
	}

	return respond(res, httplib::StatusCode::InternalServerError_500);
}

httplib::Response handleTransacao(int clienteId, const std::string & body, httplib::Response& res) {
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

	try {
		pqxx::connection conn = getConnection();
		pqxx::work txn{conn};

		std::vector<std::string> params;
		std::string clienteIdStr = pqxx::to_string(clienteId);
		std::string valorStr = pqxx::to_string(data["valor"].template get<int>()); 

		std::string query;
		if (tipo == "c") {
			query = std::vformat(
				queryCreditar,
				std::make_format_args(
					valorStr,
					clienteIdStr,
					clienteIdStr,
					valorStr,
					txn.quote(tipo),
					txn.quote(descricao)
				)
			);
		} else {
			query = std::vformat(
				queryDebitar,
				std::make_format_args(
					valorStr,
					clienteIdStr,
					clienteIdStr,
					valorStr,
					txn.quote(tipo),
					txn.quote(descricao)
				)
			);
		}

		auto [limite, saldo] = txn.query1<int, int>(query);

		json responseBody = {
			{"saldo", saldo},
			{"limite", limite}
		};

		return respond(res, httplib::StatusCode::OK_200, responseBody);
	} catch (pqxx::sql_error const &e) {
		return respond(res, httplib::StatusCode::UnprocessableContent_422);
	}

	return respond(res, httplib::StatusCode::InternalServerError_500);
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

	int portNum = std::atoi(std::getenv("SERVER_PORT"));
	std::cout << "Servidor rodando em 0.0.0.0:" << portNum << std::endl;

	server.listen("0.0.0.0", portNum);
}