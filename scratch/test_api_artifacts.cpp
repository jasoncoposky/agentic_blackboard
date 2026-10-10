#include <agentic_blackboard/Blackboard.hpp>
#include <agentic_blackboard/StorageManager.hpp>
#include <agentic_blackboard/Librarian.hpp>
#include <agentic_blackboard/ApiServer.hpp>
#include "httplib.h"
#include <nlohmann/json.hpp>
#include <iostream>
#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <chrono>
#include <thread>

using json = nlohmann::json;

int main() {
    std::cout << "=== Running C++ test_api_artifacts ===" << std::endl;

    std::string test_db = "/tmp/test_api_artifacts_db";
    std::string test_vault = "/tmp/test_api_artifacts_vault";
    std::filesystem::remove_all(test_db);
    std::filesystem::remove_all(test_vault);

    const int test_port = 18099;
    const std::string host = "127.0.0.1";

    {
        ab::Blackboard blackboard(test_db, 1);
        ab::storage::StorageManager storage_mgr(test_vault);
        auto& librarian = ab::Librarian::instance();

        ab::ApiServer::instance().start(&blackboard, test_port, host, &storage_mgr, &librarian);

        // Allow server thread to bind
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        httplib::Client cli(host, test_port);
        cli.set_connection_timeout(std::chrono::seconds(5));
        cli.set_read_timeout(std::chrono::seconds(5));

        // 1. Upload Artifact via Multipart Form
        std::string sample_data = "0123456789abcdefghijklmnopqrstuvwxyz";
        httplib::UploadFormDataItems items = {
            {"file", sample_data, "spec.md", "text/markdown"},
            {"path", "docs/spec.md", "", ""},
            {"license", "Apache-2.0", "", ""},
            {"avus", R"([{"attribute":"policy:tier","value":"hot","units":""},{"attribute":"domain","value":"science","units":""}])", "", ""}
        };

        auto res = cli.Post("/api/v1/artifacts/upload", items);
        assert(res && res->status == 200);

        auto upload_json = json::parse(res->body);
        std::string uuid = upload_json.value("uuid", "");
        assert(!uuid.empty());
        assert(upload_json["pid"] == "urn:ab:artifact:docs/spec.md");
        assert(upload_json["logical_name"] == "spec.md");
        assert(upload_json["collection_path"] == "/docs");
        assert(upload_json["license"] == "Apache-2.0");
        assert(upload_json["byte_size"] == sample_data.size());
        std::string content_hash = upload_json["content_hash"];
        assert(!content_hash.empty());

        std::cout << "[PASS] 1. Uploaded artifact with UUID: " << uuid << std::endl;

        // 2. Retrieve metadata by UUID
        res = cli.Get("/api/v1/artifacts/" + uuid);
        assert(res && res->status == 200);
        auto get_json = json::parse(res->body);
        assert(get_json["uuid"] == uuid);
        assert(get_json["pid"] == "urn:ab:artifact:docs/spec.md");
        std::cout << "[PASS] 2. Retrieved metadata by UUID" << std::endl;

        // 3. Retrieve metadata by logical path
        res = cli.Get("/api/v1/artifacts/docs/spec.md");
        assert(res && res->status == 200);
        auto path_json = json::parse(res->body);
        assert(path_json["uuid"] == uuid);

        res = cli.Get("/api/v1/artifacts/spec.md");
        assert(res && res->status == 200);
        assert(json::parse(res->body)["uuid"] == uuid);
        std::cout << "[PASS] 3. Retrieved metadata by logical path" << std::endl;

        // 4. Retrieve full content
        res = cli.Get("/api/v1/artifacts/" + uuid + "/content");
        assert(res && res->status == 200);
        assert(res->body == sample_data);
        assert(res->get_header_value("ETag") == "\"" + content_hash + "\"");
        assert(res->get_header_value("Accept-Ranges") == "bytes");
        assert(res->get_header_value("Content-Disposition").find("spec.md") != std::string::npos);
        std::cout << "[PASS] 4. Full content streaming verified with ETag and headers" << std::endl;

        // 5. Retrieve partial content (HTTP Range)
        httplib::Headers range_headers = {{"Range", "bytes=2-14"}};
        res = cli.Get("/api/v1/artifacts/" + uuid + "/content", range_headers);
        assert(res && res->status == 206);
        std::string expected_slice = sample_data.substr(2, 13); // bytes 2..14 inclusive (13 bytes)
        assert(res->body == expected_slice);
        assert(res->get_header_value("Content-Range") == "bytes 2-14/" + std::to_string(sample_data.size()));
        assert(res->get_header_value("ETag") == "\"" + content_hash + "\"");
        assert(res->get_header_value("Accept-Ranges") == "bytes");
        std::cout << "[PASS] 5. Partial content range request verified (HTTP 206, exact bytes)" << std::endl;

        // 5b. Suffix range exceeding file size: Range: bytes=-1000
        httplib::Headers suffix_range_headers = {{"Range", "bytes=-1000"}};
        res = cli.Get("/api/v1/artifacts/" + uuid + "/content", suffix_range_headers);
        assert(res && res->status == 206);
        assert(res->body == sample_data);
        assert(res->get_header_value("Content-Range") == "bytes 0-" + std::to_string(sample_data.size() - 1) + "/" + std::to_string(sample_data.size()));
        std::cout << "[PASS] 5b. Suffix range request verified" << std::endl;

        // 5c. Out-of-bounds range request: Range: bytes=9999-
        httplib::Headers oob_range_headers = {{"Range", "bytes=9999-"}};
        res = cli.Get("/api/v1/artifacts/" + uuid + "/content", oob_range_headers);
        assert(res && res->status == 416);
        assert(res->get_header_value("Content-Range") == "bytes */" + std::to_string(sample_data.size()));
        std::cout << "[PASS] 5c. Out-of-bounds range request (HTTP 416) verified" << std::endl;

        // 6. Metadata endpoint GET
        res = cli.Get("/api/v1/artifacts/" + uuid + "/metadata");
        assert(res && res->status == 200);
        auto meta_json = json::parse(res->body);
        assert(meta_json["uuid"] == uuid);
        assert(meta_json["avus"].is_array());
        std::cout << "[PASS] 6. GET /api/v1/artifacts/{uuid}/metadata verified" << std::endl;

        // 7. Metadata endpoint POST (update AVUs)
        json avu_update = {
            {"avus", json::array({
                {{"attribute", "policy:tier"}, {"value", "archive"}, {"units", ""}},
                {{"attribute", "curator"}, {"value", "alice"}, {"units", ""}}
            })}
        };
        res = cli.Post("/api/v1/artifacts/" + uuid + "/metadata", avu_update.dump(), "application/json");
        assert(res && res->status == 200);
        auto updated_meta = json::parse(res->body);
        assert(updated_meta["status"] == "OK");

        // Verify updated AVUs via metadata GET
        res = cli.Get("/api/v1/artifacts/" + uuid + "/metadata");
        assert(res && res->status == 200);
        auto verify_meta = json::parse(res->body);
        bool found_archive = false;
        bool found_curator = false;
        bool found_hot = false;
        for (const auto& a : verify_meta["avus"]) {
            if (a["attribute"] == "policy:tier" && a["value"] == "archive") found_archive = true;
            if (a["attribute"] == "policy:tier" && a["value"] == "hot") found_hot = true;
            if (a["attribute"] == "curator" && a["value"] == "alice") found_curator = true;
        }
        assert(found_archive);
        assert(found_curator);
        assert(!found_hot);
        std::cout << "[PASS] 7. POST /api/v1/artifacts/{uuid}/metadata updated AVUs" << std::endl;

        // 8. Query artifacts by AVU
        res = cli.Get("/api/v1/artifacts/query?attribute=policy:tier&value=archive");
        assert(res && res->status == 200);
        auto query_json = json::parse(res->body);
        assert(query_json.is_array());
        assert(query_json.size() == 1);
        assert(query_json[0]["uuid"] == uuid);

        res = cli.Get("/api/v1/artifacts/query?attribute=policy:tier&value=hot");
        assert(res && res->status == 200);
        assert(json::parse(res->body).empty());
        std::cout << "[PASS] 8. Query artifacts by AVU verified" << std::endl;

        // 9. Negative tests
        res = cli.Get("/api/v1/artifacts/nonexistent-uuid");
        assert(res && res->status == 404);

        res = cli.Get("/api/v1/artifacts/nonexistent-uuid/content");
        assert(res && res->status == 404);

        res = cli.Get("/api/v1/artifacts/nonexistent-uuid/metadata");
        assert(res && res->status == 404);

        res = cli.Post("/api/v1/artifacts/nonexistent-uuid/metadata", "{}", "application/json");
        assert(res && res->status == 404);

        res = cli.Post("/api/v1/artifacts/upload", "", "application/json");
        assert(res && res->status == 400);

        std::cout << "[PASS] 9. Negative tests (404 and 400) verified" << std::endl;

        // 9b. Non-artifact node type validation (CPB atom node vs ArtifactEntry)
        ab::CpbEntry atom_entry;
        atom_entry.header.uuid = "atom-node-test-999";
        atom_entry.header.origin.project_id = "proj-default";
        atom_entry.header.origin.user_id = "user:tester";
        atom_entry.payload.statement = "Test CPB atom statement";
        bool atom_ok = blackboard.commit_cpb_entry(atom_entry);
        assert(atom_ok);

        res = cli.Get("/api/v1/artifacts/atom-node-test-999");
        assert(res && res->status == 404);
        res = cli.Get("/api/v1/artifacts/atom-node-test-999/content");
        assert(res && res->status == 404);
        res = cli.Get("/api/v1/artifacts/atom-node-test-999/metadata");
        assert(res && res->status == 404);
        std::cout << "[PASS] 9b. Non-artifact node (atom) verified as 404" << std::endl;

        // 10. Upload with form license: SPDX:Apache-2.0 -> verify fair:score AVU includes +20 license points
        httplib::UploadFormDataItems lic_items = {
            {"target_path", "models/resnet.pt", "", ""},
            {"license", "SPDX:Apache-2.0", "", ""},
            {"file", "binary weights data", "resnet.pt", "application/octet-stream"}
        };
        res = cli.Post("/api/v1/artifacts/upload", lic_items);
        assert(res && res->status == 200);
        auto lic_meta = json::parse(res->body);
        assert(lic_meta["license"] == "SPDX:Apache-2.0");
        int fair_score_lic = 0;
        for (const auto& a : lic_meta["avus"]) {
            if (a["attribute"] == "fair:score") {
                fair_score_lic = std::stoi(a["value"].get<std::string>());
            }
        }
        // Score: PID (+15) + content_hash (+15) + path/logical (+10) + license (+20) = 60
        assert(fair_score_lic == 60);
        std::cout << "[PASS] 10. Upload with license and FAIR score verified (+20 license points)" << std::endl;

        ab::ApiServer::instance().stop();
        assert(ab::ApiServer::instance().librarian() == nullptr);
        assert(ab::ApiServer::instance().ingestion_service() == nullptr);
        assert(ab::ApiServer::instance().storage_manager() == nullptr);
        std::cout << "[PASS] 11. ApiServer pointer hygiene after stop() verified" << std::endl;
    }

    std::filesystem::remove_all(test_db);
    std::filesystem::remove_all(test_vault);

    std::cout << "[SUCCESS] All test_api_artifacts tests passed!" << std::endl;
    return 0;
}
