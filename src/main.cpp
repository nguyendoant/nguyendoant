// ============================================================================
// EXAMPLE USAGE AND TESTS
// ============================================================================

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include "hl7_tcp.hpp"

using namespace LowLatencyNet;

// HL7 Server that processes messages and sends ACKs
void hl7_server() {
    ConnectionConfig cfg;
    cfg.host_port = 2575;  // Standard HL7 port
    cfg.no_delay = true;
    cfg.quick_ack = true;
    cfg.low_latency = true;
    
    TCPServer server(cfg);
    
    if (!server.bind() || !server.listen()) {
        std::cerr << "Failed to start HL7 server" << std::endl;
        return;
    }
    
    std::cout << "HL7 Server listening on port " << server.get_local_port() << std::endl;
    
    size_t msg_count = 0;
    auto start = std::chrono::high_resolution_clock::now();
    
    while (server.is_running()) {
        TCPClient* client = server.accept();
        if (client) {
            std::cout << "Client connected from " << client->get_peer_ip() 
                      << ":" << client->get_peer_port() << std::endl;
            
            try {
                while (true) {
                    // Receive HL7 message
                    auto msg = client->recv_hl7_message();
                    msg_count++;
                    
                    std::cout << "Received HL7 message #" << msg_count << std::endl;
                    std::cout << "  Type: " << msg.get_message_type() << std::endl;
                    std::cout << "  Control ID: " << msg.get_control_id() << std::endl;
                    std::cout << "  Segments: " << msg.segment_count() << std::endl;
                    
                    // Send ACK
                    auto ack = HL7Message::create_ack(msg.get_control_id(), "AA");
                    client->send_hl7_message(ack);
                    
                    // Stop after processing enough messages for benchmark
                    if (msg_count >= 1000) {
                        auto end = std::chrono::high_resolution_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            end - start).count();
                        double throughput = (msg_count * 1000.0) / duration;
                        std::cout << "\nServer processed " << msg_count << " messages in " 
                                  << duration << " ms" << std::endl;
                        std::cout << "Server throughput: " << throughput << " msg/s" << std::endl;
                        server.stop();
                        break;
                    }
                }
            } catch (const std::exception& e) {
                std::cout << "Client disconnected: " << e.what() << std::endl;
            }
            
            delete client;
        }
    }
}

// HL7 Client benchmark test
void hl7_client_benchmark() {
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Wait for server
    
    ConnectionConfig cfg;
    cfg.guest_ip = "127.0.0.1";
    cfg.guest_port = 2575;
    cfg.no_delay = true;
    cfg.quick_ack = true;
    cfg.low_latency = true;
    
    TCPClient client(cfg);
    
    std::cout << "\n=== HL7 Client Benchmark ===" << std::endl;
    
    try {
        if (!client.connect()) {
            std::cerr << "Failed to connect to server" << std::endl;
            return;
        }
        
        std::cout << "Connected to HL7 server" << std::endl;
        std::cout << "Local: " << client.get_local_ip() << ":" << client.get_local_port() << std::endl;
        std::cout << "Remote: " << client.get_peer_ip() << ":" << client.get_peer_port() << std::endl;
        
        // Run latency benchmark
        std::cout << "\nRunning latency benchmark (1000 round-trips)..." << std::endl;
        auto latency_result = HL7Benchmark::run_latency_test(client, 1000);
        latency_result.print();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

// Simulate various HL7 message types
void hl7_message_simulation() {
    std::cout << "\n=== HL7 Laboratory Message Simulation ===" << std::endl;
    
    // Test 1: Blood Chemistry Panel
    std::cout << "\n1. COMPREHENSIVE METABOLIC PANEL (Blood Chemistry):" << std::endl;
    std::cout << "   Tests: Glucose, BUN, Creatinine, Electrolytes, Liver Enzymes" << std::endl;
    auto cmp = HL7Message::create_blood_chemistry_panel("PT100001", "JOHNSON^ROBERT^M");
    std::cout << "\n" << cmp.to_string() << std::endl;
    std::cout << "   Segments: " << cmp.segment_count() << std::endl;
    
    // Test 2: Complete Blood Count
    std::cout << "\n2. COMPLETE BLOOD COUNT (CBC with Differential):" << std::endl;
    std::cout << "   Tests: WBC, RBC, Hemoglobin, Hematocrit, Platelets, Differential" << std::endl;
    auto cbc = HL7Message::create_cbc_panel("PT100002", "WILLIAMS^SARAH^A");
    std::cout << "\n" << cbc.to_string() << std::endl;
    std::cout << "   Segments: " << cbc.segment_count() << std::endl;
    
    // Test 3: Urinalysis
    std::cout << "\n3. URINALYSIS (Complete with Microscopic):" << std::endl;
    std::cout << "   Tests: Physical, Chemical, and Microscopic examination" << std::endl;
    auto ua = HL7Message::create_urinalysis("PT100003", "BROWN^JAMES^T");
    std::cout << "\n" << ua.to_string() << std::endl;
    std::cout << "   Segments: " << ua.segment_count() << std::endl;
    
    // Test 4: Lipid Panel
    std::cout << "\n4. LIPID PANEL (Cardiovascular Risk):" << std::endl;
    std::cout << "   Tests: Total Cholesterol, Triglycerides, HDL, LDL, VLDL" << std::endl;
    auto lipid = HL7Message::create_lipid_panel("PT100004", "DAVIS^MARY^E");
    std::cout << "\n" << lipid.to_string() << std::endl;
    std::cout << "   Segments: " << lipid.segment_count() << std::endl;
    
    // Test 5: Thyroid Function
    std::cout << "\n5. THYROID FUNCTION PANEL:" << std::endl;
    std::cout << "   Tests: TSH, T4, T3, Free T4, Free T3" << std::endl;
    auto thyroid = HL7Message::create_thyroid_panel("PT100005", "GARCIA^MARIA^L");
    std::cout << "\n" << thyroid.to_string() << std::endl;
    std::cout << "   Segments: " << thyroid.segment_count() << std::endl;
    
    // Test 6: Coagulation Studies
    std::cout << "\n6. COAGULATION PANEL:" << std::endl;
    std::cout << "   Tests: PT, INR, PTT, Fibrinogen, D-Dimer" << std::endl;
    auto coag = HL7Message::create_coagulation_panel("PT100006", "MILLER^DAVID^R");
    std::cout << "\n" << coag.to_string() << std::endl;
    std::cout << "   Segments: " << coag.segment_count() << std::endl;
    
    // Test 7: Abnormal Results with Critical Values
    std::cout << "\n7. ABNORMAL CHEMISTRY PANEL (Critical Values):" << std::endl;
    std::cout << "   Contains multiple out-of-range values requiring attention" << std::endl;
    auto abnormal = HL7Message::create_abnormal_chemistry("PT100007", "WILSON^JOHN^S");
    std::cout << "\n" << abnormal.to_string() << std::endl;
    std::cout << "   Segments: " << abnormal.segment_count() << std::endl;
    
    // Test 8: Message Parsing
    std::cout << "\n8. MESSAGE PARSING DEMONSTRATION:" << std::endl;
    HL7Message parsed(cmp.to_string());
    std::cout << "   Message Type: " << parsed.get_message_type() << std::endl;
    std::cout << "   Control ID: " << parsed.get_control_id() << std::endl;
    std::cout << "   Total Segments: " << parsed.segment_count() << std::endl;
    
    for (size_t i = 0; i < parsed.segment_count() && i < 5; i++) {
        std::cout << "   Segment " << i << ": " << parsed.get_segment_id(i) << std::endl;
    }
    
    // Test 9: ACK message
    std::cout << "\n9. ACKNOWLEDGMENT MESSAGE:" << std::endl;
    auto ack = HL7Message::create_ack("MSG00000123", "AA");
    std::cout << "\n" << ack.to_string() << std::endl;
    
    // Summary
    std::cout << "\n=== LABORATORY PANEL SUMMARY ===" << std::endl;
    std::cout << "Available Test Panels:" << std::endl;
    std::cout << "  • Comprehensive Metabolic Panel (CMP) - 14 tests" << std::endl;
    std::cout << "  • Complete Blood Count (CBC) - 14 tests with differential" << std::endl;
    std::cout << "  • Urinalysis (UA) - Physical, chemical, microscopic" << std::endl;
    std::cout << "  • Lipid Panel - 6 cardiovascular markers" << std::endl;
    std::cout << "  • Thyroid Function Tests - 5 hormone levels" << std::endl;
    std::cout << "  • Coagulation Studies - 5 clotting factors" << std::endl;
    std::cout << "\nAll messages use HL7 v2.5 standard with proper MLLP framing" << std::endl;
}

// TLS-enabled HL7 server
void hl7_tls_server() {
    ConnectionConfig cfg;
    cfg.host_port = 2576;  // Secure HL7 port
    cfg.no_delay = true;
    
    // Enable TLS
    cfg.tls.enabled = true;
    cfg.tls.cert_file = "server.crt";
    cfg.tls.key_file = "server.key";
    cfg.tls.verify_peer = false;
    
    TCPServer server(cfg);
    
    if (!server.bind() || !server.listen()) {
        std::cerr << "Failed to start secure HL7 server" << std::endl;
        return;
    }
    
    std::cout << "Secure HL7 Server listening on port " << server.get_local_port() << std::endl;
    
    while (server.is_running()) {
        TCPClient* client = server.accept();
        if (client) {
            std::cout << "Secure client connected" << std::endl;
            std::cout << "TLS Version: " << client->get_tls_version() << std::endl;
            std::cout << "Cipher: " << client->get_tls_cipher() << std::endl;
            
            try {
                auto msg = client->recv_hl7_message();
                std::cout << "Received encrypted HL7 message" << std::endl;
                
                auto ack = HL7Message::create_ack(msg.get_control_id(), "AA");
                client->send_hl7_message(ack);
                
                server.stop();
            } catch (const std::exception& e) {
                std::cout << "Error: " << e.what() << std::endl;
            }
            
            delete client;
        }
    }
}

// Stress test with multiple concurrent connections
void hl7_stress_test() {
    std::cout << "\n=== HL7 Stress Test ===" << std::endl;
    std::cout << "Testing with multiple concurrent connections..." << std::endl;
    
    const int NUM_CLIENTS = 10;
    const int MSGS_PER_CLIENT = 100;
    
    std::vector<std::thread> threads;
    std::atomic<size_t> total_messages{0};
    std::atomic<double> total_time_ms{0};
    
    for (int i = 0; i < NUM_CLIENTS; i++) {
        threads.emplace_back([i, &total_messages, &total_time_ms]() {
            ConnectionConfig cfg;
            cfg.guest_ip = "127.0.0.1";
            cfg.guest_port = 2575;
            cfg.no_delay = true;
            
            TCPClient client(cfg);
            
            try {
                if (client.connect()) {
                    auto start = std::chrono::high_resolution_clock::now();
                    
                    for (int j = 0; j < MSGS_PER_CLIENT; j++) {
                        auto msg = HL7Message::create_oru_result(
                            "PT" + std::to_string(i * 1000 + j),
                            "GLU",
                            std::to_string(80 + (j % 40)),
                            "mg/dL",
                            "70-100"
                        );
                        
                        if (client.send_hl7_message(msg)) {
                            auto ack = client.recv_hl7_message();
                            total_messages++;
                        }
                    }
                    
                    auto end = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        end - start).count();
                    double old_total = total_time_ms.load();
                    while (!total_time_ms.compare_exchange_weak(old_total, old_total + duration)) {}
                }
            } catch (const std::exception& e) {
                std::cerr << "Client " << i << " error: " << e.what() << std::endl;
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "\nStress Test Results:" << std::endl;
    std::cout << "Total messages sent: " << total_messages << std::endl;
    std::cout << "Average time per client: " << (total_time_ms / NUM_CLIENTS) << " ms" << std::endl;
    std::cout << "Total throughput: " 
              << (total_messages * 1000.0) / (total_time_ms / NUM_CLIENTS) 
              << " msg/s" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " [server|client|simulate|tls-server|stress]" << std::endl;
        return 1;
    }
    
    std::string mode = argv[1];
    
    if (mode == "server") {
        hl7_server();
    } else if (mode == "client") {
        hl7_client_benchmark();
    } else if (mode == "simulate") {
        hl7_message_simulation();
    } else if (mode == "tls-server") {
        hl7_tls_server();
    } else if (mode == "stress") {
        // Start server in background
        std::thread server_thread(hl7_server);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        hl7_stress_test();
        
        server_thread.join();
    } else if (mode == "benchmark") {
        // Run full benchmark suite
        std::thread server_thread(hl7_server);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        hl7_client_benchmark();
        
        server_thread.join();
    } else {
        std::cout << "Unknown mode: " << mode << std::endl;
        return 1;
    }
    
    return 0;
}

// Compilation:
// g++ -std=c++11 -pthread hl7_server.cpp -o hl7_server -lssl -lcrypto

// Usage examples:
// ./hl7_server simulate          # Show HL7 message examples
// ./hl7_server server             # Start HL7 server
// ./hl7_server client             # Run client benchmark
// ./hl7_server benchmark          # Run full benchmark suite
// ./hl7_server stress             # Run stress test with multiple clients
// ./hl7_server tls-server         # Start TLS-enabled server

// Generate test certificates:
// openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -days 365 -nodes -subj "/CN=localhost"
