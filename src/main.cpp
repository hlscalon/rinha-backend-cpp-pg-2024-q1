#include <iostream>
#include <thread>
#include <chrono>
#include <pqxx/pqxx>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <memory>

#include "libs/httplib.h"
#include "libs/json.hpp"

using json = nlohmann::json;

class DBPool {
public:
	DBPool(std::string dsn, int poolSize = 10) {
		createPool(dsn, poolSize);
	}

	std::shared_ptr<pqxx::connection> connection() {
		std::unique_lock<std::mutex> lock(mutex_);

		// se a pool está vazia, espera a notificação
		while (pool_.empty()) {
			condition_.wait(lock);
		}

		// pega uma conexão
		std::shared_ptr<pqxx::connection> conn = pool_.front();

		// remove da pool
		pool_.pop();

		return conn;
	}

	void freeConnection(std::shared_ptr<pqxx::connection> conn) {
		std::unique_lock<std::mutex> lock_(mutex_);

		// insere uma nova conexão na pool
		pool_.push(conn);

		// unlock no mutex
		lock_.unlock();

		// notifica a thread que está esperando
		condition_.notify_one();
	}

private:
	std::mutex mutex_;
	std::condition_variable condition_;
	std::queue<std::shared_ptr<pqxx::connection>> pool_;

	void createPool(std::string dsn, int poolSize) {
		std::lock_guard<std::mutex> locker(mutex_);

		for (int i = 0; i < poolSize; i++) {
			pool_.emplace(std::make_shared<pqxx::connection>(dsn));
		}
	}
};

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

httplib::Response handleExtrato(std::shared_ptr<DBPool> pool, int clienteId, httplib::Response& res) {
	auto conn = pool->connection();

	try {
		// reinterpret_cast<pqxx::connection&>(*conn.get())
		pqxx::work txn{*conn.get()};
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

		if (nrow > 1) {
			ultimasTransacoes.erase(ultimasTransacoes.end() - 1);
		}

		txn.commit();
		pool->freeConnection(conn);

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
		pool->freeConnection(conn);

		return respond(res, httplib::StatusCode::UnprocessableContent_422);
	} catch (...) {
		pool->freeConnection(conn);
	}

	return respond(res, httplib::StatusCode::UnprocessableContent_422);
}

httplib::Response handleTransacao(std::shared_ptr<DBPool> pool, int clienteId, const std::string & body, httplib::Response& res) {
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

	auto conn = pool->connection();

	try {
		// reinterpret_cast<pqxx::connection&>(*conn.get())
		pqxx::work txn{*conn.get()};

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
					valorStr,
					clienteIdStr,
					valorStr,
					txn.quote(tipo),
					txn.quote(descricao)
				)
			);
		}

		auto [limite, saldo] = txn.query1<int, int>(query);
		txn.commit();
		pool->freeConnection(conn);

		json responseBody = {
			{"saldo", saldo},
			{"limite", limite}
		};

		return respond(res, httplib::StatusCode::OK_200, responseBody);
	} catch (pqxx::sql_error const &e) {
		pool->freeConnection(conn);

		return respond(res, httplib::StatusCode::UnprocessableContent_422);
	} catch (...) {
		pool->freeConnection(conn);
	}

	return respond(res, httplib::StatusCode::UnprocessableContent_422);
}

std::shared_ptr<DBPool> createPool() {
	char * dbName = std::getenv("DB_NAME");
	char * dbUser = std::getenv("DB_USER");
	char * dbPassword = std::getenv("DB_PASSWORD");
	char * dbHost = std::getenv("DB_HOST");
	char * dbPort = std::getenv("DB_PORT");

	if (!dbName || !dbUser || !dbPassword || !dbHost || !dbPort) {
		throw std::runtime_error("Variáveis não informadas");
	}

	const std::string dsn = std::format(
		"dbname={} user={} password={} host={} port={}",
		dbName,
		dbUser,
		dbPassword,
		dbHost,
		dbPort
	);

	int poolSize = 10;
	int retries = 1;

	do {
		std::cout << "Tentando conectar banco... tentativa " << retries << std::endl;

		try {
			return std::make_shared<DBPool>(dsn, poolSize);
		} catch (...) {
			retries++;

			std::this_thread::sleep_for(std::chrono::seconds(5));
		}

	} while (retries < 20);

	throw std::runtime_error("Erro ao obter conexão banco");
}

int main() {
	std::shared_ptr<DBPool> pool = createPool();
	httplib::Server server;

	server.Get("/clientes/:id/extrato", [&](const httplib::Request& req, httplib::Response& res) {
		int clienteId = std::stoi(req.path_params.at("id"));
		if (!isValidClienteId(clienteId)) {
			return respond(res, httplib::StatusCode::NotFound_404);
		}

		return handleExtrato(pool, clienteId, res);
	});

	server.Post("/clientes/:id/transacoes", [&](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader &content_reader) {
		int clienteId = std::stoi(req.path_params.at("id"));
		if (!isValidClienteId(clienteId)) {
			return respond(res, httplib::StatusCode::NotFound_404);
		}

		std::string body;
		content_reader([&](const char *data, size_t data_length) {
			body.append(data, data_length);
			return true;
		});

		return handleTransacao(pool, clienteId, body, res);
	});

	char * serverPort = std::getenv("SERVER_PORT");
	if (!serverPort) {
		std::cerr << "Informe a porta do servidor via SERVER_PORT" << std::endl;
		return -1;
	}

	int portNum = std::atoi(serverPort);
	std::cout << "Servidor rodando em 0.0.0.0:" << portNum << std::endl;

	server.listen("0.0.0.0", portNum);
}