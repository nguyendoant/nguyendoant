#ifndef LOW_LATENCY_TCP_HL7_HPP
#define LOW_LATENCY_TCP_HL7_HPP

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdexcept>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <sstream>
#include <chrono>
#include <iomanip>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

namespace LowLatencyNet {

// HL7 Constants
const char HL7_START_BLOCK = 0x0B;  // Vertical Tab
const char HL7_END_BLOCK = 0x1C;    // File Separator
const char HL7_CARRIAGE_RETURN = 0x0D;

const char HL7_FIELD_SEP = '|';
const char HL7_COMPONENT_SEP = '^';
const char HL7_REPETITION_SEP = '~';
const char HL7_ESCAPE_CHAR = '\\';
const char HL7_SUBCOMPONENT_SEP = '&';

struct TLSConfig {
    bool enabled = false;
    std::string cert_file = "";
    std::string key_file = "";
    std::string ca_file = "";
    std::string ca_path = "";
    bool verify_peer = true;
    bool verify_hostname = true;
    std::string ciphers = "HIGH:!aNULL:!MD5:!RC4";
    int min_tls_version = TLS1_2_VERSION;
    int max_tls_version = TLS1_3_VERSION;
    bool session_reuse = true;
    std::string alpn_protocols = "";
};

struct ConnectionConfig {
    std::string host_ip = "0.0.0.0";
    int host_port = 0;
    std::string guest_ip = "";
    int guest_port = 0;
    TLSConfig tls;
    bool no_delay = true;
    bool quick_ack = true;
    bool cork = false;
    int send_buffer_size = 256 * 1024;
    int recv_buffer_size = 256 * 1024;
    int send_timeout = 0;
    int recv_timeout = 0;
    bool reuse_addr = true;
    bool reuse_port = false;
    bool keep_alive = true;
    int keep_alive_idle = 60;
    int keep_alive_interval = 10;
    int keep_alive_count = 5;
    bool non_blocking = false;
    int priority = 0;
    bool low_latency = true;
};

// HL7 Message Parser and Builder
class HL7Message {
private:
    std::string raw_message;
    std::vector<std::vector<std::string>> segments;
    
    std::vector<std::string> split(const std::string& str, char delim) {
        std::vector<std::string> result;
        std::stringstream ss(str);
        std::string item;
        while (std::getline(ss, item, delim)) {
            result.push_back(item);
        }
        return result;
    }
    
public:
    HL7Message() = default;
    
    explicit HL7Message(const std::string& msg) : raw_message(msg) {
        parse(msg);
    }
    
    void parse(const std::string& msg) {
        raw_message = msg;
        segments.clear();
        
        // Split by segment delimiter (CR)
        auto segment_strings = split(msg, HL7_CARRIAGE_RETURN);
        
        for (const auto& seg_str : segment_strings) {
            if (seg_str.empty()) continue;
            
            // Split by field separator
            auto fields = split(seg_str, HL7_FIELD_SEP);
            segments.push_back(fields);
        }
    }
    
    std::string get_segment_id(size_t idx) const {
        if (idx < segments.size() && !segments[idx].empty()) {
            return segments[idx][0];
        }
        return "";
    }
    
    std::string get_field(size_t seg_idx, size_t field_idx) const {
        if (seg_idx < segments.size() && field_idx < segments[seg_idx].size()) {
            return segments[seg_idx][field_idx];
        }
        return "";
    }
    
    std::string get_message_type() const {
        // MSH segment, field 9 (message type)
        if (!segments.empty() && segments[0][0] == "MSH") {
            return get_field(0, 8);  // Adjusted for 0-based indexing
        }
        return "";
    }
    
    std::string get_control_id() const {
        // MSH segment, field 10 (message control ID)
        if (!segments.empty() && segments[0][0] == "MSH") {
            return get_field(0, 9);
        }
        return "";
    }
    
    std::string to_string() const {
        return raw_message;
    }
    
    size_t segment_count() const {
        return segments.size();
    }
    
    // Build HL7 acknowledgment message
    static HL7Message create_ack(const std::string& msg_control_id, 
                                  const std::string& ack_code = "AA") {
        std::stringstream ss;
        
        // MSH segment
        ss << "MSH|^~\\&|RECEIVER|FACILITY|SENDER|FACILITY|"
           << get_timestamp() << "||ACK|" << msg_control_id << "|P|2.5"
           << HL7_CARRIAGE_RETURN;
        
        // MSA segment
        ss << "MSA|" << ack_code << "|" << msg_control_id
           << HL7_CARRIAGE_RETURN;
        
        return HL7Message(ss.str());
    }
    
    // Create comprehensive blood chemistry panel (CMP)
    static HL7Message create_blood_chemistry_panel(const std::string& patient_id,
                                                    const std::string& patient_name) {
        std::stringstream ss;
        std::string control_id = generate_control_id();
        std::string observation_time = get_timestamp();
        
        // MSH - Message Header
        ss << "MSH|^~\\&|LAB|CENTRAL_LAB|EHR|HOSPITAL|"
           << observation_time << "||ORU^R01|" << control_id << "|P|2.5"
           << HL7_CARRIAGE_RETURN;
        
        // PID - Patient Identification
        ss << "PID|1||" << patient_id << "^^^HOSPITAL^MR||" << patient_name 
           << "||19750515|M|||123 MAIN ST^^CITY^ST^12345||555-1234|||S||"
           << patient_id << "|||||||||||"
           << HL7_CARRIAGE_RETURN;
        
        // ORC - Common Order
        ss << "ORC|RE|" << control_id << "|" << control_id << "||CM||||"
           << observation_time << "|||1234^SMITH^JOHN^A^^DR"
           << HL7_CARRIAGE_RETURN;
        
        // OBR - Observation Request
        ss << "OBR|1|" << control_id << "|" << control_id 
           << "|CMP^COMPREHENSIVE METABOLIC PANEL^L|||" << observation_time
           << "|||||||||1234^SMITH^JOHN^A^^DR||||||" << observation_time 
           << "|||F"
           << HL7_CARRIAGE_RETURN;
        
        // Blood Chemistry Results
        add_obx(ss, 1, "GLU", "Glucose", "95", "mg/dL", "70-100", "N");
        add_obx(ss, 2, "BUN", "Blood Urea Nitrogen", "18", "mg/dL", "7-20", "N");
        add_obx(ss, 3, "CREAT", "Creatinine", "1.0", "mg/dL", "0.7-1.3", "N");
        add_obx(ss, 4, "NA", "Sodium", "140", "mmol/L", "136-145", "N");
        add_obx(ss, 5, "K", "Potassium", "4.2", "mmol/L", "3.5-5.1", "N");
        add_obx(ss, 6, "CL", "Chloride", "102", "mmol/L", "98-107", "N");
        add_obx(ss, 7, "CO2", "Carbon Dioxide", "24", "mmol/L", "22-29", "N");
        add_obx(ss, 8, "CA", "Calcium", "9.5", "mg/dL", "8.5-10.5", "N");
        add_obx(ss, 9, "TP", "Total Protein", "7.2", "g/dL", "6.0-8.3", "N");
        add_obx(ss, 10, "ALB", "Albumin", "4.0", "g/dL", "3.5-5.0", "N");
        add_obx(ss, 11, "TBIL", "Total Bilirubin", "0.8", "mg/dL", "0.1-1.2", "N");
        add_obx(ss, 12, "ALP", "Alkaline Phosphatase", "72", "U/L", "30-120", "N");
        add_obx(ss, 13, "AST", "Aspartate Aminotransferase", "28", "U/L", "10-40", "N");
        add_obx(ss, 14, "ALT", "Alanine Aminotransferase", "32", "U/L", "7-56", "N");
        
        return HL7Message(ss.str());
    }
    
    // Create complete blood count (CBC) with differential
    static HL7Message create_cbc_panel(const std::string& patient_id,
                                       const std::string& patient_name) {
        std::stringstream ss;
        std::string control_id = generate_control_id();
        std::string observation_time = get_timestamp();
        
        ss << "MSH|^~\\&|LAB|HEMATOLOGY|EHR|HOSPITAL|"
           << observation_time << "||ORU^R01|" << control_id << "|P|2.5"
           << HL7_CARRIAGE_RETURN;
        
        ss << "PID|1||" << patient_id << "^^^HOSPITAL^MR||" << patient_name 
           << "||19850320|F"
           << HL7_CARRIAGE_RETURN;
        
        ss << "ORC|RE|" << control_id << "|" << control_id << "||CM||||"
           << observation_time
           << HL7_CARRIAGE_RETURN;
        
        ss << "OBR|1|" << control_id << "|" << control_id 
           << "|CBC^COMPLETE BLOOD COUNT^L|||" << observation_time
           << "|||||||||1234^JONES^MARY^B^^DR||||||" << observation_time 
           << "|||F"
           << HL7_CARRIAGE_RETURN;
        
        // CBC Results
        add_obx(ss, 1, "WBC", "White Blood Cell Count", "7.5", "10*3/uL", "4.0-11.0", "N");
        add_obx(ss, 2, "RBC", "Red Blood Cell Count", "4.8", "10*6/uL", "4.2-5.4", "N");
        add_obx(ss, 3, "HGB", "Hemoglobin", "14.2", "g/dL", "12.0-16.0", "N");
        add_obx(ss, 4, "HCT", "Hematocrit", "42.5", "%", "37.0-47.0", "N");
        add_obx(ss, 5, "MCV", "Mean Corpuscular Volume", "88", "fL", "80-100", "N");
        add_obx(ss, 6, "MCH", "Mean Corpuscular Hemoglobin", "29.6", "pg", "27-33", "N");
        add_obx(ss, 7, "MCHC", "Mean Corpuscular Hgb Concentration", "33.4", "g/dL", "32-36", "N");
        add_obx(ss, 8, "RDW", "Red Cell Distribution Width", "13.2", "%", "11.5-14.5", "N");
        add_obx(ss, 9, "PLT", "Platelet Count", "250", "10*3/uL", "150-400", "N");
        
        // Differential
        add_obx(ss, 10, "NEUT%", "Neutrophils", "60", "%", "40-70", "N");
        add_obx(ss, 11, "LYMPH%", "Lymphocytes", "30", "%", "20-40", "N");
        add_obx(ss, 12, "MONO%", "Monocytes", "7", "%", "2-10", "N");
        add_obx(ss, 13, "EOS%", "Eosinophils", "2", "%", "0-5", "N");
        add_obx(ss, 14, "BASO%", "Basophils", "1", "%", "0-2", "N");
        
        return HL7Message(ss.str());
    }
    
    // Create urinalysis results
    static HL7Message create_urinalysis(const std::string& patient_id,
                                        const std::string& patient_name) {
        std::stringstream ss;
        std::string control_id = generate_control_id();
        std::string observation_time = get_timestamp();
        
        ss << "MSH|^~\\&|LAB|URINALYSIS_LAB|EHR|HOSPITAL|"
           << observation_time << "||ORU^R01|" << control_id << "|P|2.5"
           << HL7_CARRIAGE_RETURN;
        
        ss << "PID|1||" << patient_id << "^^^HOSPITAL^MR||" << patient_name 
           << "||19900707|M"
           << HL7_CARRIAGE_RETURN;
        
        ss << "ORC|RE|" << control_id << "|" << control_id << "||CM||||"
           << observation_time
           << HL7_CARRIAGE_RETURN;
        
        ss << "OBR|1|" << control_id << "|" << control_id 
           << "|UA^URINALYSIS^L|||" << observation_time
           << "|||||||||5678^BROWN^ROBERT^C^^DR||||||" << observation_time 
           << "|||F"
           << HL7_CARRIAGE_RETURN;
        
        // Physical Examination
        add_obx(ss, 1, "COLOR", "Color", "Yellow", "", "Yellow", "N", "ST");
        add_obx(ss, 2, "APPEAR", "Appearance", "Clear", "", "Clear", "N", "ST");
        add_obx(ss, 3, "SPGRAV", "Specific Gravity", "1.015", "", "1.005-1.030", "N");
        
        // Chemical Examination
        add_obx(ss, 4, "PH", "pH", "6.0", "", "5.0-8.0", "N");
        add_obx(ss, 5, "PROT", "Protein", "Negative", "", "Negative", "N", "ST");
        add_obx(ss, 6, "GLUC", "Glucose", "Negative", "", "Negative", "N", "ST");
        add_obx(ss, 7, "KET", "Ketones", "Negative", "", "Negative", "N", "ST");
        add_obx(ss, 8, "BLOOD", "Blood", "Negative", "", "Negative", "N", "ST");
        add_obx(ss, 9, "BILI", "Bilirubin", "Negative", "", "Negative", "N", "ST");
        add_obx(ss, 10, "URO", "Urobilinogen", "0.2", "mg/dL", "0.1-1.0", "N");
        add_obx(ss, 11, "NIT", "Nitrite", "Negative", "", "Negative", "N", "ST");
        add_obx(ss, 12, "LEUK", "Leukocyte Esterase", "Negative", "", "Negative", "N", "ST");
        
        // Microscopic Examination
        add_obx(ss, 13, "WBC_URINE", "WBCs", "0-2", "/HPF", "0-5", "N", "ST");
        add_obx(ss, 14, "RBC_URINE", "RBCs", "0-1", "/HPF", "0-3", "N", "ST");
        add_obx(ss, 15, "EPITH", "Epithelial Cells", "Few", "/HPF", "Few", "N", "ST");
        add_obx(ss, 16, "BACT", "Bacteria", "None", "", "None", "N", "ST");
        add_obx(ss, 17, "CRYST", "Crystals", "None", "", "None", "N", "ST");
        
        return HL7Message(ss.str());
    }
    
    // Create lipid panel
    static HL7Message create_lipid_panel(const std::string& patient_id,
                                         const std::string& patient_name) {
        std::stringstream ss;
        std::string control_id = generate_control_id();
        std::string observation_time = get_timestamp();
        
        ss << "MSH|^~\\&|LAB|CHEMISTRY|EHR|HOSPITAL|"
           << observation_time << "||ORU^R01|" << control_id << "|P|2.5"
           << HL7_CARRIAGE_RETURN;
        
        ss << "PID|1||" << patient_id << "^^^HOSPITAL^MR||" << patient_name 
           << "||19650815|M"
           << HL7_CARRIAGE_RETURN;
        
        ss << "ORC|RE|" << control_id << "|" << control_id << "||CM||||"
           << observation_time
           << HL7_CARRIAGE_RETURN;
        
        ss << "OBR|1|" << control_id << "|" << control_id 
           << "|LIPID^LIPID PANEL^L|||" << observation_time
           << "|||||||||2345^WILSON^SUSAN^D^^DR||||||" << observation_time 
           << "|||F"
           << HL7_CARRIAGE_RETURN;
        
        add_obx(ss, 1, "CHOL", "Total Cholesterol", "195", "mg/dL", "<200", "N");
        add_obx(ss, 2, "TRIG", "Triglycerides", "120", "mg/dL", "<150", "N");
        add_obx(ss, 3, "HDL", "HDL Cholesterol", "55", "mg/dL", ">40", "N");
        add_obx(ss, 4, "LDL", "LDL Cholesterol (calculated)", "116", "mg/dL", "<100", "H");
        add_obx(ss, 5, "VLDL", "VLDL Cholesterol", "24", "mg/dL", "5-40", "N");
        add_obx(ss, 6, "CHOL/HDL", "Cholesterol/HDL Ratio", "3.5", "", "<5.0", "N");
        
        return HL7Message(ss.str());
    }
    
    // Create thyroid function tests
    static HL7Message create_thyroid_panel(const std::string& patient_id,
                                           const std::string& patient_name) {
        std::stringstream ss;
        std::string control_id = generate_control_id();
        std::string observation_time = get_timestamp();
        
        ss << "MSH|^~\\&|LAB|ENDOCRINOLOGY|EHR|HOSPITAL|"
           << observation_time << "||ORU^R01|" << control_id << "|P|2.5"
           << HL7_CARRIAGE_RETURN;
        
        ss << "PID|1||" << patient_id << "^^^HOSPITAL^MR||" << patient_name 
           << "||19820425|F"
           << HL7_CARRIAGE_RETURN;
        
        ss << "ORC|RE|" << control_id << "|" << control_id << "||CM||||"
           << observation_time
           << HL7_CARRIAGE_RETURN;
        
        ss << "OBR|1|" << control_id << "|" << control_id 
           << "|THYROID^THYROID FUNCTION PANEL^L|||" << observation_time
           << "|||||||||3456^DAVIS^MICHAEL^E^^DR||||||" << observation_time 
           << "|||F"
           << HL7_CARRIAGE_RETURN;
        
        add_obx(ss, 1, "TSH", "Thyroid Stimulating Hormone", "2.5", "uIU/mL", "0.4-4.0", "N");
        add_obx(ss, 2, "T4", "Thyroxine (T4)", "8.5", "ug/dL", "5.0-12.0", "N");
        add_obx(ss, 3, "T3", "Triiodothyronine (T3)", "120", "ng/dL", "80-200", "N");
        add_obx(ss, 4, "FT4", "Free T4", "1.2", "ng/dL", "0.8-1.8", "N");
        add_obx(ss, 5, "FT3", "Free T3", "3.0", "pg/mL", "2.3-4.2", "N");
        
        return HL7Message(ss.str());
    }
    
    // Create coagulation panel
    static HL7Message create_coagulation_panel(const std::string& patient_id,
                                               const std::string& patient_name) {
        std::stringstream ss;
        std::string control_id = generate_control_id();
        std::string observation_time = get_timestamp();
        
        ss << "MSH|^~\\&|LAB|COAGULATION_LAB|EHR|HOSPITAL|"
           << observation_time << "||ORU^R01|" << control_id << "|P|2.5"
           << HL7_CARRIAGE_RETURN;
        
        ss << "PID|1||" << patient_id << "^^^HOSPITAL^MR||" << patient_name 
           << "||19700910|M"
           << HL7_CARRIAGE_RETURN;
        
        ss << "ORC|RE|" << control_id << "|" << control_id << "||CM||||"
           << observation_time
           << HL7_CARRIAGE_RETURN;
        
        ss << "OBR|1|" << control_id << "|" << control_id 
           << "|COAG^COAGULATION PANEL^L|||" << observation_time
           << "|||||||||4567^ANDERSON^LINDA^F^^DR||||||" << observation_time 
           << "|||F"
           << HL7_CARRIAGE_RETURN;
        
        add_obx(ss, 1, "PT", "Prothrombin Time", "12.5", "sec", "11.0-13.5", "N");
        add_obx(ss, 2, "INR", "International Normalized Ratio", "1.0", "", "0.8-1.2", "N");
        add_obx(ss, 3, "PTT", "Partial Thromboplastin Time", "30", "sec", "25-35", "N");
        add_obx(ss, 4, "FIBRIN", "Fibrinogen", "320", "mg/dL", "200-400", "N");
        add_obx(ss, 5, "DIMER", "D-Dimer", "0.3", "ug/mL FEU", "<0.5", "N");
        
        return HL7Message(ss.str());
    }
    
    // Create comprehensive metabolic panel with abnormal values
    static HL7Message create_abnormal_chemistry(const std::string& patient_id,
                                                const std::string& patient_name) {
        std::stringstream ss;
        std::string control_id = generate_control_id();
        std::string observation_time = get_timestamp();
        
        ss << "MSH|^~\\&|LAB|STAT_LAB|EHR|HOSPITAL|"
           << observation_time << "||ORU^R01|" << control_id << "|P|2.5"
           << HL7_CARRIAGE_RETURN;
        
        ss << "PID|1||" << patient_id << "^^^HOSPITAL^MR||" << patient_name 
           << "||19551203|M"
           << HL7_CARRIAGE_RETURN;
        
        ss << "ORC|RE|" << control_id << "|" << control_id << "||CM||||"
           << observation_time
           << HL7_CARRIAGE_RETURN;
        
        ss << "OBR|1|" << control_id << "|" << control_id 
           << "|CMP^COMPREHENSIVE METABOLIC PANEL^L|||" << observation_time
           << "|||||||||5678^MARTINEZ^CARLOS^G^^DR||||||" << observation_time 
           << "|||F"
           << HL7_CARRIAGE_RETURN;
        
        // Abnormal results marked with H (high) or L (low)
        add_obx(ss, 1, "GLU", "Glucose", "245", "mg/dL", "70-100", "H");
        add_obx(ss, 2, "BUN", "Blood Urea Nitrogen", "45", "mg/dL", "7-20", "H");
        add_obx(ss, 3, "CREAT", "Creatinine", "2.8", "mg/dL", "0.7-1.3", "H");
        add_obx(ss, 4, "NA", "Sodium", "128", "mmol/L", "136-145", "L");
        add_obx(ss, 5, "K", "Potassium", "5.8", "mmol/L", "3.5-5.1", "H");
        add_obx(ss, 6, "CL", "Chloride", "95", "mmol/L", "98-107", "L");
        add_obx(ss, 7, "CO2", "Carbon Dioxide", "18", "mmol/L", "22-29", "L");
        add_obx(ss, 8, "CA", "Calcium", "11.2", "mg/dL", "8.5-10.5", "H");
        add_obx(ss, 9, "ALT", "Alanine Aminotransferase", "180", "U/L", "7-56", "H");
        add_obx(ss, 10, "AST", "Aspartate Aminotransferase", "220", "U/L", "10-40", "H");
        
        // Add note segment for critical values
        ss << "NTE|1||CRITICAL: Potassium level requires immediate attention"
           << HL7_CARRIAGE_RETURN;
        ss << "NTE|2||Patient shows signs of renal impairment and hyperglycemia"
           << HL7_CARRIAGE_RETURN;
        
        return HL7Message(ss.str());
    }
    
private:
    static std::string get_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y%m%d%H%M%S");
        return ss.str();
    }
    
    static std::string generate_control_id() {
        static std::atomic<int> counter{1};
        std::stringstream ss;
        ss << "MSG" << std::setfill('0') << std::setw(8) << counter++;
        return ss.str();
    }
    
    static void add_obx(std::stringstream& ss, int seq, 
                       const std::string& id, const std::string& name,
                       const std::string& value, const std::string& units,
                       const std::string& ref_range, const std::string& abnormal_flag,
                       const std::string& value_type = "NM") {
        ss << "OBX|" << seq << "|" << value_type << "|" << id << "^" << name 
           << "^L||" << value << "|" << units << "|" << ref_range 
           << "|" << abnormal_flag << "|||F"
           << HL7_CARRIAGE_RETURN;
    }

public:
    // Create ORU (Observation Result Unsolicited) message
    static HL7Message create_oru_result(const std::string& patient_id,
                                         const std::string& test_name,
                                         const std::string& test_value,
                                         const std::string& units,
                                         const std::string& ref_range) {
        std::stringstream ss;
        std::string control_id = generate_control_id();

        // MSH - Message Header
        ss << "MSH|^~\\&|LAB|FACILITY|EHR|HOSPITAL|"
           << get_timestamp() << "||ORU^R01|" << control_id << "|P|2.5"
           << HL7_CARRIAGE_RETURN;
        
        // PID - Patient Identification
        ss << "PID|1||" << patient_id << "^^^FACILITY^MR||DOE^JOHN^||19800101|M"
           << HL7_CARRIAGE_RETURN;
        
        // OBR - Observation Request
        ss << "OBR|1|" << control_id << "|" << control_id << "|"
           << test_name << "|||" << get_timestamp()
           << HL7_CARRIAGE_RETURN;
        
        // OBX - Observation Result
        ss << "OBX|1|NM|" << test_name << "||" << test_value 
           << "|" << units << "|" << ref_range << "|N|||F"
           << HL7_CARRIAGE_RETURN;
        
        return HL7Message(ss.str());
    }
    
    // Create ADT (Admission/Discharge/Transfer) message
    static HL7Message create_adt_a01(const std::string& patient_id,
                                     const std::string& patient_name) {
        std::stringstream ss;
        std::string control_id = generate_control_id();
        
        // MSH - Message Header
        ss << "MSH|^~\\&|ADT|FACILITY|EHR|HOSPITAL|"
           << get_timestamp() << "||ADT^A01|" << control_id << "|P|2.5"
           << HL7_CARRIAGE_RETURN;
        
        // EVN - Event Type
        ss << "EVN|A01|" << get_timestamp()
           << HL7_CARRIAGE_RETURN;
        
        // PID - Patient Identification
        ss << "PID|1||" << patient_id << "^^^FACILITY^MR||" << patient_name 
           << "||19800101|M|||123 MAIN ST^^CITY^ST^12345"
           << HL7_CARRIAGE_RETURN;
        
        // PV1 - Patient Visit
        ss << "PV1|1|I|ICU^101^01||||DOC123^SMITH^JANE|||MED||||||||V123"
           << HL7_CARRIAGE_RETURN;
        
        return HL7Message(ss.str());
    }
};

class TLSContext {
private:
    SSL_CTX* ctx = nullptr;
    bool is_server;
    
public:
    TLSContext(bool server, const TLSConfig& config) : is_server(server) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        
        const SSL_METHOD* method = is_server ? TLS_server_method() : TLS_client_method();
        ctx = SSL_CTX_new(method);
        if (!ctx) throw std::runtime_error("Failed to create SSL context");
        
        SSL_CTX_set_min_proto_version(ctx, config.min_tls_version);
        SSL_CTX_set_max_proto_version(ctx, config.max_tls_version);
        
        if (!config.ciphers.empty()) {
            SSL_CTX_set_cipher_list(ctx, config.ciphers.c_str());
        }
        
        if (!config.cert_file.empty()) {
            if (SSL_CTX_use_certificate_file(ctx, config.cert_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
                SSL_CTX_free(ctx);
                throw std::runtime_error("Failed to load certificate file");
            }
        }
        
        if (!config.key_file.empty()) {
            if (SSL_CTX_use_PrivateKey_file(ctx, config.key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
                SSL_CTX_free(ctx);
                throw std::runtime_error("Failed to load private key file");
            }
            if (!SSL_CTX_check_private_key(ctx)) {
                SSL_CTX_free(ctx);
                throw std::runtime_error("Private key does not match certificate");
            }
        }
        
        if (!config.ca_file.empty() || !config.ca_path.empty()) {
            const char* ca_file = config.ca_file.empty() ? nullptr : config.ca_file.c_str();
            const char* ca_path = config.ca_path.empty() ? nullptr : config.ca_path.c_str();
            if (!SSL_CTX_load_verify_locations(ctx, ca_file, ca_path)) {
                SSL_CTX_free(ctx);
                throw std::runtime_error("Failed to load CA certificates");
            }
        }
        
        int verify_mode = config.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
        if (is_server && config.verify_peer) {
            verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
        SSL_CTX_set_verify(ctx, verify_mode, nullptr);
        
        if (config.session_reuse) {
            SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_BOTH);
        }
    }
    
    ~TLSContext() {
        if (ctx) SSL_CTX_free(ctx);
    }
    
    SSL_CTX* get() { return ctx; }
};

class TCPSocket {
    friend class TCPServer;
protected:
    int sock_fd = -1;
    SSL* ssl = nullptr;
    ConnectionConfig config;
    std::string peer_ip;
    int peer_port = 0;
    
    void apply_config() {
        if (sock_fd < 0) return;
        
        int flag = config.no_delay ? 1 : 0;
        setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        
        #ifdef TCP_QUICKACK
        flag = config.quick_ack ? 1 : 0;
        setsockopt(sock_fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
        #endif
        
        #ifdef TCP_CORK
        flag = config.cork ? 1 : 0;
        setsockopt(sock_fd, IPPROTO_TCP, TCP_CORK, &flag, sizeof(flag));
        #endif
        
        setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF, &config.send_buffer_size, sizeof(config.send_buffer_size));
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &config.recv_buffer_size, sizeof(config.recv_buffer_size));
        
        if (config.send_timeout > 0) {
            struct timeval tv;
            tv.tv_sec = config.send_timeout / 1000;
            tv.tv_usec = (config.send_timeout % 1000) * 1000;
            setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        
        if (config.recv_timeout > 0) {
            struct timeval tv;
            tv.tv_sec = config.recv_timeout / 1000;
            tv.tv_usec = (config.recv_timeout % 1000) * 1000;
            setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
        
        flag = config.reuse_addr ? 1 : 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
        
        #ifdef SO_REUSEPORT
        flag = config.reuse_port ? 1 : 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag));
        #endif
        
        flag = config.keep_alive ? 1 : 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
        
        if (config.priority > 0) {
            setsockopt(sock_fd, SOL_SOCKET, SO_PRIORITY, &config.priority, sizeof(config.priority));
        }
        
        if (config.low_latency) {
            int tos = IPTOS_LOWDELAY;
            setsockopt(sock_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        }
        
        if (config.non_blocking) {
            int flags = fcntl(sock_fd, F_GETFL, 0);
            fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
    
public:
    TCPSocket() = default;
    explicit TCPSocket(const ConnectionConfig& cfg) : config(cfg) {}
    
    virtual ~TCPSocket() { close(); }
    
    void set_config(const ConnectionConfig& cfg) {
        config = cfg;
        apply_config();
    }
    
    ssize_t send(const void* data, size_t len, int flags = 0) {
        if (ssl) return SSL_write(ssl, data, len);
        return ::send(sock_fd, data, len, flags | MSG_NOSIGNAL);
    }
    
    ssize_t recv(void* buffer, size_t len, int flags = 0) {
        if (ssl) return SSL_read(ssl, buffer, len);
        return ::recv(sock_fd, buffer, len, flags);
    }
    
    void close() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (sock_fd >= 0) {
            ::close(sock_fd);
            sock_fd = -1;
        }
    }
    
    bool is_open() const { return sock_fd >= 0; }
    bool is_tls_enabled() const { return ssl != nullptr; }
    
    std::string get_tls_version() const {
        if (!ssl) return "";
        return SSL_get_version(ssl);
    }
    
    std::string get_tls_cipher() const {
        if (!ssl) return "";
        const char* cipher = SSL_get_cipher(ssl);
        return cipher ? cipher : "";
    }
    
    int get_fd() const { return sock_fd; }
    std::string get_peer_ip() const { return peer_ip; }
    int get_peer_port() const { return peer_port; }
    
    std::string get_local_ip() const {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        if (getsockname(sock_fd, (struct sockaddr*)&addr, &len) == 0) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
            return std::string(ip);
        }
        return "";
    }
    
    int get_local_port() const {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        if (getsockname(sock_fd, (struct sockaddr*)&addr, &len) == 0) {
            return ntohs(addr.sin_port);
        }
        return 0;
    }
    
    // HL7-specific send/receive with MLLP framing
    bool send_hl7_message(const HL7Message& msg) {
        std::string raw = msg.to_string();
        std::string framed;
        framed += HL7_START_BLOCK;
        framed += raw;
        framed += HL7_END_BLOCK;
        framed += HL7_CARRIAGE_RETURN;
        
        ssize_t sent = send(framed.c_str(), framed.length());
        return sent == (ssize_t)framed.length();
    }
    
    HL7Message recv_hl7_message() {
        std::string buffer;
        char ch;
        bool started = false;
        
        // Look for start block
        while (recv(&ch, 1) == 1) {
            if (ch == HL7_START_BLOCK) {
                started = true;
                break;
            }
        }
        
        if (!started) {
            throw std::runtime_error("HL7 start block not found");
        }
        
        // Read until end block
        while (recv(&ch, 1) == 1) {
            if (ch == HL7_END_BLOCK) {
                break;
            }
            buffer += ch;
        }
        
        // Read trailing CR
        recv(&ch, 1);
        
        return HL7Message(buffer);
    }
};

class TCPClient : public TCPSocket {
private:
    TLSContext* tls_ctx = nullptr;
    
public:
    TCPClient() = default;
    explicit TCPClient(const ConnectionConfig& cfg) : TCPSocket(cfg) {
        if (cfg.tls.enabled) {
            tls_ctx = new TLSContext(false, cfg.tls);
        }
    }
    
    ~TCPClient() {
        if (tls_ctx) delete tls_ctx;
    }
    
    bool connect(const std::string& host, int port) {
        std::string target_host = host.empty() ? config.guest_ip : host;
        int target_port = (port == 0) ? config.guest_port : port;
        
        if (target_host.empty() || target_port == 0) {
            throw std::runtime_error("Invalid target host or port");
        }
        
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            throw std::runtime_error("Failed to create socket");
        }
        
        apply_config();
        
        if (!config.host_ip.empty() && config.host_ip != "0.0.0.0") {
            struct sockaddr_in local_addr;
            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin_family = AF_INET;
            local_addr.sin_port = htons(config.host_port);
            
            if (inet_pton(AF_INET, config.host_ip.c_str(), &local_addr.sin_addr) <= 0) {
                close();
                throw std::runtime_error("Invalid local address");
            }
            
            if (::bind(sock_fd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
                close();
                throw std::runtime_error("Failed to bind to local address");
            }
        }
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(target_port);
        
        if (inet_pton(AF_INET, target_host.c_str(), &addr.sin_addr) <= 0) {
            close();
            throw std::runtime_error("Invalid address");
        }
        
        if (::connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close();
            return false;
        }
        
        peer_ip = target_host;
        peer_port = target_port;
        
        if (config.tls.enabled && tls_ctx) {
            ssl = SSL_new(tls_ctx->get());
            if (!ssl) {
                close();
                throw std::runtime_error("Failed to create SSL object");
            }
            
            SSL_set_fd(ssl, sock_fd);
            
            if (config.tls.verify_hostname) {
                SSL_set_tlsext_host_name(ssl, target_host.c_str());
                X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
                X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                X509_VERIFY_PARAM_set1_host(param, target_host.c_str(), 0);
            }
            
            if (SSL_connect(ssl) <= 0) {
                close();
                throw std::runtime_error("TLS handshake failed");
            }
            
            if (config.tls.verify_peer) {
                if (SSL_get_verify_result(ssl) != X509_V_OK) {
                    close();
                    throw std::runtime_error("Certificate verification failed");
                }
            }
        }
        
        return true;
    }
    
    bool connect() {
        return connect(config.guest_ip, config.guest_port);
    }
};

class TCPServer : public TCPSocket {
private:
    std::atomic<bool> running{false};
    TLSContext* tls_ctx = nullptr;
    
public:
    TCPServer() = default;
    explicit TCPServer(const ConnectionConfig& cfg) : TCPSocket(cfg) {
        if (cfg.tls.enabled) {
            tls_ctx = new TLSContext(true, cfg.tls);
        }
    }
    
    ~TCPServer() {
        if (tls_ctx) delete tls_ctx;
    }
    
    bool bind(int port, const std::string& addr = "0.0.0.0") {
        std::string bind_addr = addr.empty() ? config.host_ip : addr;
        int bind_port = (port == 0) ? config.host_port : port;
        
        if (bind_port == 0) {
            throw std::runtime_error("Invalid bind port");
        }
        
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            throw std::runtime_error("Failed to create socket");
        }
        
        apply_config();
        
        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(bind_port);
        
        if (bind_addr == "0.0.0.0" || bind_addr.empty()) {
            serv_addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            if (inet_pton(AF_INET, bind_addr.c_str(), &serv_addr.sin_addr) <= 0) {
                close();
                throw std::runtime_error("Invalid address");
            }
        }
        
        if (::bind(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            close();
            return false;
        }
        
        return true;
    }
    
    bool bind() {
        return bind(config.host_port, config.host_ip);
    }
    
    bool listen(int backlog = 128) {
        if (::listen(sock_fd, backlog) < 0) {
            return false;
        }
        running = true;
        return true;
    }
    
    TCPClient* accept() {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = ::accept(sock_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            return nullptr;
        }
        
        char peer_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, peer_ip_str, sizeof(peer_ip_str));
        int peer_port_val = ntohs(client_addr.sin_port);
        
        if (!config.guest_ip.empty() && config.guest_ip != std::string(peer_ip_str)) {
            ::close(client_fd);
            return nullptr;
        }
        
        if (config.guest_port != 0 && config.guest_port != peer_port_val) {
            ::close(client_fd);
            return nullptr;
        }
        
        TCPClient* client = new TCPClient(config);
        client->sock_fd = client_fd;
        client->peer_ip = peer_ip_str;
        client->peer_port = peer_port_val;
        client->apply_config();
        
        if (config.tls.enabled && tls_ctx) {
            client->ssl = SSL_new(tls_ctx->get());
            if (!client->ssl) {
                delete client;
                return nullptr;
            }
            
            SSL_set_fd(client->ssl, client_fd);
            
            if (SSL_accept(client->ssl) <= 0) {
                delete client;
                return nullptr;
            }
        }
        
        return client;
    }
    
    void stop() { running = false; }
    bool is_running() const { return running; }
};

// Benchmark utilities
struct BenchmarkResult {
    size_t message_count;
    double total_time_ms;
    double avg_latency_us;
    double min_latency_us;
    double max_latency_us;
    double throughput_msgs_per_sec;
    size_t total_bytes;
    double bandwidth_mbps;
    
    void print() const {
        std::cout << "\n=== Benchmark Results ===" << std::endl;
        std::cout << "Messages sent:     " << message_count << std::endl;
        std::cout << "Total time:        " << total_time_ms << " ms" << std::endl;
        std::cout << "Avg latency:       " << avg_latency_us << " µs" << std::endl;
        std::cout << "Min latency:       " << min_latency_us << " µs" << std::endl;
        std::cout << "Max latency:       " << max_latency_us << " µs" << std::endl;
        std::cout << "Throughput:        " << throughput_msgs_per_sec << " msg/s" << std::endl;
        std::cout << "Bandwidth:         " << bandwidth_mbps << " Mbps" << std::endl;
    }
};

class HL7Benchmark {
public:
    static BenchmarkResult run_latency_test(TCPClient& client, size_t iterations = 1000) {
        BenchmarkResult result = {0};
        std::vector<double> latencies;
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (size_t i = 0; i < iterations; i++) {
            auto msg = HL7Message::create_oru_result(
                "PT" + std::to_string(i), 
                "GLU", 
                std::to_string(90 + (i % 50)),
                "mg/dL",
                "70-100"
            );
            
            auto msg_start = std::chrono::high_resolution_clock::now();
            
            if (!client.send_hl7_message(msg)) {
                std::cerr << "Failed to send message" << std::endl;
                break;
            }
            
            try {
                auto ack = client.recv_hl7_message();
                
                auto msg_end = std::chrono::high_resolution_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                    msg_end - msg_start).count();
                latencies.push_back(latency);
                
                result.total_bytes += msg.to_string().length() + ack.to_string().length();
            } catch (const std::exception& e) {
                std::cerr << "Error receiving ACK: " << e.what() << std::endl;
                break;
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        result.message_count = latencies.size();
        result.total_time_ms = total_time;
        
        if (!latencies.empty()) {
            double sum = 0;
            result.min_latency_us = latencies[0];
            result.max_latency_us = latencies[0];
            
            for (auto lat : latencies) {
                sum += lat;
                if (lat < result.min_latency_us) result.min_latency_us = lat;
                if (lat > result.max_latency_us) result.max_latency_us = lat;
            }
            
            result.avg_latency_us = sum / latencies.size();
            result.throughput_msgs_per_sec = (result.message_count * 1000.0) / total_time;
            result.bandwidth_mbps = (result.total_bytes * 8.0) / (total_time * 1000.0);
        }
        
        return result;
    }
    
    static BenchmarkResult run_throughput_test(TCPClient& client, size_t iterations = 10000) {
        BenchmarkResult result = {0};
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (size_t i = 0; i < iterations; i++) {
            auto msg = HL7Message::create_adt_a01(
                "PT" + std::to_string(i),
                "DOE^JOHN^M"
            );
            
            if (!client.send_hl7_message(msg)) {
                std::cerr << "Failed to send message" << std::endl;
                break;
            }
            
            result.total_bytes += msg.to_string().length();
            result.message_count++;
            
            // For throughput test, we don't wait for ACK
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        result.total_time_ms = total_time;
        result.throughput_msgs_per_sec = (result.message_count * 1000.0) / total_time;
        result.bandwidth_mbps = (result.total_bytes * 8.0) / (total_time * 1000.0);
        
        return result;
    }
};

} // namespace LowLatencyNet

#endif // LOW_LATENCY_TCP_HL7_HPP

