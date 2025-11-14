// Unit tests for ConnectionType enum and helper functions
#include "catch_amalgamated.hpp"
#include "network/connection_types.hpp"

using namespace unicity::network;

TEST_CASE("ConnectionType - String conversion", "[network][connection_types]") {
    SECTION("INBOUND converts to 'inbound'") {
        CHECK(ConnectionTypeAsString(ConnectionType::INBOUND) == "inbound");
    }

    SECTION("OUTBOUND converts to 'outbound'") {
        CHECK(ConnectionTypeAsString(ConnectionType::OUTBOUND) == "outbound");
    }

    SECTION("MANUAL converts to 'manual'") {
        CHECK(ConnectionTypeAsString(ConnectionType::MANUAL) == "manual");
    }

    SECTION("FEELER converts to 'feeler'") {
        CHECK(ConnectionTypeAsString(ConnectionType::FEELER) == "feeler");
    }

    SECTION("Invalid value converts to 'unknown'") {
        // Cast to an invalid enum value
        ConnectionType invalid = static_cast<ConnectionType>(999);
        CHECK(ConnectionTypeAsString(invalid) == "unknown");
    }
}

TEST_CASE("ConnectionType - Enum values", "[network][connection_types]") {
    SECTION("Enum values are distinct") {
        CHECK(ConnectionType::INBOUND != ConnectionType::OUTBOUND);
        CHECK(ConnectionType::INBOUND != ConnectionType::MANUAL);
        CHECK(ConnectionType::INBOUND != ConnectionType::FEELER);
        CHECK(ConnectionType::OUTBOUND != ConnectionType::MANUAL);
        CHECK(ConnectionType::OUTBOUND != ConnectionType::FEELER);
        CHECK(ConnectionType::MANUAL != ConnectionType::FEELER);
    }

    SECTION("Can assign and compare enum values") {
        ConnectionType type1 = ConnectionType::INBOUND;
        ConnectionType type2 = ConnectionType::INBOUND;
        ConnectionType type3 = ConnectionType::OUTBOUND;

        CHECK(type1 == type2);
        CHECK(type1 != type3);
    }
}

TEST_CASE("ConnectionType - Usage patterns", "[network][connection_types]") {
    SECTION("Can use in switch statements") {
        auto get_description = [](ConnectionType type) -> std::string {
            switch (type) {
            case ConnectionType::INBOUND:
                return "Connection initiated by peer";
            case ConnectionType::OUTBOUND:
                return "Default connection type";
            case ConnectionType::MANUAL:
                return "User-requested connection";
            case ConnectionType::FEELER:
                return "Short-lived test connection";
            default:
                return "Unknown";
            }
        };

        CHECK(get_description(ConnectionType::INBOUND) == "Connection initiated by peer");
        CHECK(get_description(ConnectionType::OUTBOUND) == "Default connection type");
        CHECK(get_description(ConnectionType::MANUAL) == "User-requested connection");
        CHECK(get_description(ConnectionType::FEELER) == "Short-lived test connection");
    }

    SECTION("String representation is consistent") {
        // Calling multiple times should give same result
        CHECK(ConnectionTypeAsString(ConnectionType::INBOUND) ==
              ConnectionTypeAsString(ConnectionType::INBOUND));
        CHECK(ConnectionTypeAsString(ConnectionType::FEELER) ==
              ConnectionTypeAsString(ConnectionType::FEELER));
    }
}
