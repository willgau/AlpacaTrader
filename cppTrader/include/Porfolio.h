#pragma once


#include "common.h"
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <iostream>
#include "myboost.h"
#include "secrets_local.h"

class Portfolio {

public:

	static Portfolio& getInstance(std::string iName = "Default", double iCash = 100000.0) {
		static Portfolio instance(iName, iCash);
		return instance;
	}

	static asio::awaitable<void> poll_account_forever(std::string host, std::string port);

	std::string getName() const { return mName; }

	double getCash() const { return mCash; }

private:
	Portfolio(std::string iName, double iCash) : mName{ iName }, mCash{ iCash } {};

	static awaitable<nlohmann::json> alpaca_get_account(std::string host, std::string port, ssl::context& tls_ctx);
	
    
	~Portfolio() = default;
    Portfolio(const Portfolio&) = delete;
    Portfolio& operator=(const Portfolio&) = delete;

	std::string mName;
	double mCash;

};