#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <Mahi/Gui.hpp>
#include <Mahi/Util.hpp>
#include "implot_internal.h"
#include <unordered_map>
#include <string>
#include <cstring>
#include <string.h>
#include <arpa/inet.h>

/// JSON includes
#include <iomanip>
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;
using std::to_string;

using namespace mahi::gui;
using namespace mahi::util;

double toDouble(const char* s, int start, int stop) {
    unsigned long long int m = 1;
    double ret = 0;
    for (int i = stop; i >= start; i--) {
        ret += (s[i] - '0') * m;
        m *= 10;
    }
    return ret;
}

void error(const char *msg)
{
    perror(msg);
    exit(0);
}

// utility structure for realtime plot
struct ScrollingBuffer {
    int MaxSize;
    int Offset;
    ImVector<ImVec2> Data;
    ScrollingBuffer(int max_size = 2000) {
        MaxSize = max_size;
        Offset  = 0;
        Data.reserve(MaxSize);
    }
    void AddPoint(float x, float y) {
        if (Data.size() < MaxSize)
            Data.push_back(ImVec2(x,y));
        else {
            Data[Offset] = ImVec2(x,y);
            Offset =  (Offset + 1) % MaxSize;
        }
    }
    void Erase() {
        if (Data.size() > 0) {
            Data.shrink(0);
            Offset  = 0;
        }
    }
};

// utility structure for realtime plot
struct RollingBuffer {
    float Span;
    ImVector<ImVec2> Data;
    RollingBuffer() {
        Span = 10.0f;
        Data.reserve(2000);
    }
    void AddPoint(float x, float y) {
        float xmod = fmodf(x, Span);
        if (!Data.empty() && xmod < Data.back().x)
            Data.shrink(0);
        Data.push_back(ImVec2(xmod, y));
    }
};

// Make the UI compact because there are so many fields
static void PushStyleCompact()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, (float)(int)(style.FramePadding.y * 0.70f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, (float)(int)(style.ItemSpacing.y * 0.70f)));
}

static void PopStyleCompact()
{
    ImGui::PopStyleVar(2);
}

// Helper to display a little (?) mark which shows a tooltip when hovered.
// In your own code you may want to display an actual icon if you are using a merged icon fonts (see docs/FONTS.md)
static void HelpMarker(const char* desc)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Candlestick External Function(s)
/*void PlotCandlestick(const char* label_id, double* xs, double* opens, double* closes, double* lows, double* highs, int count, bool tooltip = true, float width_percent = 0.25f, ImVec4 bullCol = ImVec4(0,1,0,1), ImVec4 bearCol = ImVec4(1,0,0,1));*/


// ImPlot stuff
namespace MyImPlot {

template <typename T>
int BinarySearch(const T* arr, int l, int r, T x) {
    if (r >= l) {
        int mid = l + (r - l) / 2;
        if (arr[mid] == x)
            return mid;
        if (arr[mid] > x)
            return BinarySearch(arr, l, mid - 1, x);
        return BinarySearch(arr, mid + 1, r, x);
    }
    return -1;
}
}

//=============================================================================
// MAHI LOG WRITER
//============================================================================

template <class Formatter>
class GuiLogWritter : public Writer {
public:
    GuiLogWritter(Severity max_severity = Debug) : Writer(max_severity), logs(65536) {}

    virtual void write(const LogRecord& record) override {
        auto log = std::pair<Severity, std::string>(record.get_severity(), Formatter::format(record));
        logs.push_back(log);
    }
    RingBuffer<std::pair<Severity, std::string>> logs;
};

static GuiLogWritter<TxtFormatter> writer;

//=============================================================================
// MAIN CLASS
//============================================================================

class Calculon : public Application {
public:
    ImGuiTextFilter filter;
    std::unordered_map<Severity, Color> colors = {
        {None    , Grays::Gray50},
        {Fatal   , Reds::Crimson},
        {Error   , Pinks::HotPink},
        {Warning , Yellows::Yellow},
        {Info    , Whites::White},
        {Verbose , Greens::LightGreen},
        {Debug   , Cyans::Cyan}
    };
    
    std::unordered_map<int, std::string> message_code = {
        {0, "misc"},
        {1, "position"},
        {2, "account_value"},
        {6, "orderStatus"}
    };
    
    // 640x480 px window
    Calculon() {//: Application(360,270,"Calculon") {
        // add writer to MAHI_LOG
        if (MahiLogger) {
            MahiLogger->add_writer(&writer);
            MahiLogger->set_max_severity(Debug);
        }
        LOG((Severity)5) << "Application Started";
    }
    
    // Default server login params
    char host[64] = "94.204.205.121";
    int port = 4044;
    int port2 = 4088;
    char char_port[8] = "4044";
    char char_port2[8] = "4088";
    char password[64] = "Can/u/C/M3?";  // NOTE: DONT USE PLAIN TEXT.
    
    // Account Information
    double netLiq = 0;
    double fundsAvailable = 0;
    
    struct liq_t {
        double p_val;
        int t;
    };
    std::vector<liq_t> pv;
    std::vector<liq_t> pv_c;
    
    // PnL-Single - Struct Vector
    struct PnL_row {
        int pos = 0;
        double PnL_daily = 0.00;
        double PnL_unrealized = 0.00;
        double value = 0.00;
    };
    // Key eq. Ticker
    std::unordered_map<std::string, PnL_row> PnL;
    
    std::unordered_map<int,std::string> R2S = {
        {0,"UAL"},
        {1,"DAL"},
        {2,"AAL"},
        {3,"LUV"},
        //{4,"RCL"},
        //{5,"CCL"},
        {4,"XOM"},
        {5,"CVX"},
        {6,"COP"},
        {7,"DVN"},
        {8,"BP"},
        {9,"USO"},
        {10,"DBO"},
        {11,"OIH"},
        {12,"IEZ"},
        {13,"XLE"},
        {14,"VDE"},
        {15,"FENY"},
        {16,"ICLN"},
        {17,"PBW"},
        {18,"LMT"},
        {19,"NOC"},
        {20,"MRO"},
        {21,"OXY"},
        {22,"FANG"},
        {23,"PXD"},
        {24,"NTR"},
        {25,"MOS"},
        {26,"CF"},
        {27,"ICL"},
        {28,"RCL"},
        {29,"CCL"},
        {30,"EOG"},
        {31,"PSX"},
        {32,"VLO"},
        {33,"KMI"},
        {34,"HES"},
        {35,"UCO"},
        {36,"SCO"},
        {37,"IXC"},
        {38,"IYE"},
        {39,"XLE"},
        {40,"XOP"},
        {41,"QCLN"},
        {42,"WWE"},
        {43,"LYV"},
        {44,"MSGS"}
    };
    
    /// JSON object
    json j;
    
    /// yyyy-MM-dd
    std::string ymd;
    
    /// 1 day ~ 32 KB
    double dates[1440];
    double opens[1440];
    double highs[1440];
    double lows[1440];
    double closes[1440];
    
    struct candle {
        double t;
        double open;
        double high;
        double low;
        double close;
    };
    
    int bookmark = 0;
    
    double * t; // Will likely need to cast inputs to double
    double * open_b;
    double * high;
    double * low;
    double * close;
    
    // Points to [i] in dates[i],opens[i],closes[i],etc.
    struct candle_ptr {
        double * t;
        double * open;
        double * high;
        double * low;
        double * close;
    };
    
    // Socket Client Vars
    int sockfd, portno, n;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    char buffer[256];
    
    int Connect(const char *host, int port) {
        portno = port;
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        server = gethostbyname(host);
        
        if (server == NULL) {
            fprintf(stderr,"ERROR, no such host\n");
        }
        
        bzero((char *) &serv_addr, sizeof(serv_addr));
        //std::memset((char *) &serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        //std::memcpy ((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
        bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
        serv_addr.sin_port = htons(portno);
        
        if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
            perror("ERROR connecting");
            return 1;
            error("ERROR connecting");
        } else {
            printf("Outbound connection Successful!\n");
        }
        //std::memset(buffer,0,sizeof(buffer));
        bzero(buffer,256);
        
        std::thread t2(&Calculon::f2,this);
        t2.detach();
        return 0;
    }
    
    // Outgoing Socket Client (2) Vars
    int sockfd_out, portno2, n2 = 0;
    struct sockaddr_in serv_addr2;
    struct hostent *server2;
    char buffer2[256];
    
    int ConnectOut(const char *host, int port) {
        portno2 = port;//atoi(argv[2]);
        sockfd_out = socket(AF_INET, SOCK_STREAM, 0);
        server2 = gethostbyname(host);//gethostbyname(argv[1]);
        
        if (server2 == NULL) {
            fprintf(stderr,"ERROR, no such host\n");
            return 1;
        }
        
        bzero((char *) &serv_addr2, sizeof(serv_addr2));
        serv_addr2.sin_family = AF_INET;
        bcopy((char *)server2->h_addr, (char *)&serv_addr2.sin_addr.s_addr, server2->h_length);
        serv_addr2.sin_port = htons(portno2);
        
        if (connect(sockfd_out,(struct sockaddr *) &serv_addr2,sizeof(serv_addr2)) < 0) {
            fprintf(stderr,"OUT: ERROR connecting\n");
            return 1;
        }
        //std::thread t3(&Calculon::SocketWrite,this);
        //t3.detach();
        return 0;
    }
    
    void SocketWrite(std::string msg) {
        bzero(buffer2,256);
        strncpy(buffer2,msg.c_str(),255);
        n2 = write(sockfd_out,buffer2,strlen(buffer2));
        
        if (n2 < 0) {
            ::close(sockfd_out);
            printf("[Closed]\n");
            fprintf(stderr,"SocketWrite(): ERROR writing to socket\n");
        }
    }
    
    void PopupWrapper(int i) {
        ImGui::OpenPopup("Options");
        
        if (ImGui::BeginPopup("Options")) {
            ImGui::Text("test\n");
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    
    // TO CLEAR OUT HOGGED PORTS:
    // lsof -P | grep ':4000' | awk '{print $2}' | xargs kill -9
    void f2() {
        bool EXIT_FUNC = false;
        while (EXIT_FUNC == false) {
            bzero(buffer,256);
            n = read(sockfd,buffer,255);
            if (n < 0) {
                //error("ERROR reading from socket");
                fprintf(stderr,"ERROR reading from socket\n");
                EXIT_FUNC = true;
                //LOG((Severity)2) << "perror(msg) = " << perror("Generic Message");
            }
            
            char c = buffer[0];     // severity
            int i = c-48;           // char to int conversion
            char *ptr = buffer;
            ptr += 2;
            
            int key = ptr[0] - 48;     // message_code key
            std::vector<std::string> pos_list = {"symbol:", "position:", "avgCost:"};
            int pos_cnt = 0;
            const char delim[2] = ":";
            //const char delim_alt[2] = ".";
            char *token;
            int pos;
            double dailyPnL, unrealizedPnL, realizedPnL, value;
            std::string str_buff, stk, str_tmp;
            std::vector<std::string> strv;
            switch (key) {
                case 0: // "misc"
                    LOG((Severity)0) << "case 0";
                    ptr++;
                    LOG((Severity)i) << ptr;
                    break;
                case 1: // "position"
                    LOG((Severity)0) << "case 1";
                    ptr++;
                    token = strtok(ptr, delim); // get the first token
                    while( token != NULL ) {
                        if (pos_cnt <= 3) {
                            str_buff.append(pos_list[pos_cnt]);   
                        } else {
                            str_buff.append("?:");
                        }
                        str_buff.append(token);
                        str_buff.append(" ");
                        token = strtok(NULL, delim);
                        pos_cnt++;
                    }
                    LOG((Severity)i) << str_buff;
                    break;
                case 2: // PnL_total
                    LOG((Severity)0) << "case 2";
                    ptr++;
                    token = strtok(ptr, delim);
                    dailyPnL = atof(token);
                    token = strtok(NULL, delim);
                    unrealizedPnL = atof(token);
                    token = strtok(NULL, delim);
                    // may or may not need realizedPnL
                    realizedPnL = atof(token);
                    token = strtok(NULL, delim);
                    // Now we need to put it somewhere
                    LOG((Severity)0) << "dailyPnL:" << dailyPnL << ", unrealizedPnL:" << unrealizedPnL << ", realizedPnL:" << realizedPnL << "\n";
                    break;
                case 3: // PnL_single
                    LOG((Severity)0) << "case 3";
                    ptr++;
                    token = strtok(ptr, delim);
                    stk.append(token);
                    token = strtok(NULL, delim);
                    pos = atoi(token);
                    token = strtok(NULL, delim);
                    dailyPnL = atof(token);
                    token = strtok(NULL, delim);
                    unrealizedPnL = atof(token);
                    token = strtok(NULL, delim);
                    value = atof(token);
                    if (!isfinite(unrealizedPnL))
                        unrealizedPnL = 0.00;
                    PnL[stk] = {pos,dailyPnL,unrealizedPnL,value};
                    LOG((Severity)0) << "PnL[" << stk << "] = {" << pos << "," << dailyPnL << "," << unrealizedPnL << "," << value << "}";
                    break;
                case 4:
                    LOG((Severity)0) << "case 4";
                    ptr++;
                    token = strtok(ptr, delim);
                    netLiq = atof(token);
                    int time_sec;
                    epoch(time_sec);
                    
                    // Just plot all netLiq - best data is more data
                    liq_t pv_t;
                    pv_t.p_val = netLiq;
                    epoch(pv_t.t);
                    pv.push_back(pv_t);
                    
                    LOG((Severity)0) << "pv_v: " << pv.back().p_val << ", pv_t: " << pv.back().t;
                    LOG((Severity)0) << "netLiq = " << netLiq;
                    break;
                case 5:
                    LOG((Severity)0) << "case 5";
                    ptr++;
                    token = strtok(ptr, delim);
                    fundsAvailable = atof(token);
                    LOG((Severity)0) << "fundsAvailable = " << fundsAvailable;
                    break;
                case 6: // orderStatus()
                    // sid:oid:status:filled:remain:avgfill:
                    LOG((Severity)0) << "case 6";
                    ptr++;
                    str_tmp.append("-> 6");
                    token = std::strtok(ptr, delim);
                    while (token) {
                        str_tmp.append("-");
                        str_tmp.append(token);
                        //strv.push_back(token);
                        token = std::strtok(nullptr, delim);
                    }
                    // do something
                    LOG((Severity)0) << str_tmp;
                    //LOG((Severity)0) << strv[0] << strv[1] << strv[2] << strv[3] << strv[4] << strv[5];
                    break;
            }
            
            n = write(sockfd,"message received",32);
            if (n < 0) {
                ::close(sockfd);
                //error("ERROR writing to socket");
                fprintf(stderr,"ERROR writing to socket\n");
                EXIT_FUNC = true;
            }
            
        }
    }
    
    std::string date_string() {
        std::time_t rawtime;
        std::tm* timeinfo;
        char buffer [80];

        std::time(&rawtime);
        timeinfo = std::localtime(&rawtime);

        std::strftime(buffer,80,"%Y-%m-%d",timeinfo);
        std::string str = buffer;
        return str;
    }
    
    /*double epoch_time() {
        unsigned long milliseconds_since_epoch =
        std::chrono::duration_cast<std::chrono::milliseconds>
            (std::chrono::system_clock::now().time_since_epoch()).count();
        double t = milliseconds_since_epoch;
        return t;
    }*/
    
    void epoch(double &ref) {
        unsigned long milliseconds_since_epoch =
        std::chrono::duration_cast<std::chrono::milliseconds>
            (std::chrono::system_clock::now().time_since_epoch()).count();
        double t = milliseconds_since_epoch;
        ref = t;
    }
    
    void epoch(int &ref) {
        unsigned long milliseconds_since_epoch =
        std::chrono::duration_cast<std::chrono::milliseconds>
            (std::chrono::system_clock::now().time_since_epoch()).count();
        int t = milliseconds_since_epoch / 1000;
        ref = t;
    }
    
    void SetupChart() {
        // get date yyyy-mm-dd
        //ymd = "2021-09-23";
        ymd = date_string();
        //ymd.append(date_string());
        
        // read from file
        std::ifstream i("data/BalanceHistory.json");
        i >> j;
        
        // get size
        auto s = j[ymd].size();
        
        if (s > 0) {
            for (int i = 0; i < s; i++) {
                dates[i] = j[ymd][i]["t"];
                opens[i] = j[ymd][i]["open"];
                highs[i] = j[ymd][i]["high"];
                lows[i] = j[ymd][i]["low"];
                closes[i] = j[ymd][i]["close"];
                bookmark +=1;
            }
        }
        
        std::thread t3(&Calculon::f3,this);
        t3.detach();
    }
    
    void set_ptr(candle_ptr &ref, int n)
    {
        // May need to replace dots w/ arrows - Ex: (.)'s becomes (->)'s
        ref.t = dates;
        ref.open = opens;
        ref.high = highs;
        ref.low = lows;
        ref.close = closes;
        
        t = ref.t;
        open_b = ref.open;
        high = ref.high;
        low = ref.low;
        close = ref.close;
        
        ref.t += n;
        ref.open += n;
        ref.high += n;
        ref.low += n;
        ref.close += n;
    }
    
    /// loops continuously to get current candle
    void f3()
    {
        /// This block gets an index to arrays[] relative to this very miniute
        int time;
        epoch(time);
        //int idx = time % 86400 / 60;
        //int idx = j[ymd].size() - 1;
        int idx = bookmark;
        candle_ptr now;
        candle cndl;
        
        while (idx < 1440)
        {
            
            idx += 1;
            
            //std::cout << "idx: " << idx << "\n";
            //set_ptr(now, 1);
            //epoch(time);
            //*now.t = time - time % 60;
        
            // set open(t) to close(t-1) if possible
            auto s = j[ymd].size();
            double json_t;
            if (s == 0) {
                json_t = time - time % 60;
            } else {
                json_t = j[ymd][s-1]["t"];
            }
           
            json_t += 60;
            cndl.t = json_t;
            //double json_close = j[ymd][s-1]["close"];
            double json_close = time;
            //*now.open = (round(json_t - *now.t) == 60) ? json_close : netLiq;
            //cndl.open = (round(cndl.t - json_t) == 60) ? json_close : netLiq;
            cndl.open = netLiq;
            //*high = *now.open;
            cndl.high = cndl.open;
            cndl.low = cndl.open;
            cndl.close = cndl.open;
            //low = *now.open;
            
            dates[idx] = cndl.t;
            opens[idx] = cndl.open;
            highs[idx] = cndl.high;
            lows[idx] = cndl.low;
            closes[idx] = cndl.close;
            
            while (time - cndl.t <= 60) {
                if (netLiq > cndl.high) {
                    cndl.high = netLiq;
                    
                }
                if (netLiq < *now.low) {
                    cndl.low = netLiq;
                }
                
                // set close
                cndl.close = netLiq;
                
                dates[idx] = cndl.t;
                opens[idx] = cndl.open;
                highs[idx] = cndl.high;
                lows[idx] = cndl.low;
                closes[idx] = cndl.close;
                
                // recalculate time(s)
                epoch(time);
            }
            
            
            
            /*j[ymd][s]["t"] = *t;
            j[ymd][s]["open"] = *now.open;
            j[ymd][s]["high"] = *high;
            j[ymd][s]["low"] = *low;
            j[ymd][s]["close"] = *close;*/
            j[ymd][s] = {{"t", cndl.t},{"open", cndl.open},{"high", cndl.high},{"low", cndl.low},{"close", cndl.close}};
            
            //*now.t = *now.t + 60;
            j[ymd][s]["t"] = cndl.t;
            cndl.t += 60;
            //double tmp = j[ymd][s]["t"];
            //*now.t = tmp + 60;
           
            
            // write candle to (another) json file
            std::ofstream outfile("data/output.json");
            outfile << std::setw(4) << j << std::endl;
        }
    }
    
    // Override update (called once per frame)
    void update() override {
        // App logic and/or ImGui code goes here
        //ImGui::SetNextWindowSize(ImVec2(900, 1440), ImGuiCond_FirstUseEver);
        ImGui::Begin("Calculon Graphical Interface", &open);
        
        if (ImGui::Button("Connect")) {
            int conn_resp = Connect(host,port);
            if (conn_resp == 0) {
                // Connection Success
                LOG((Severity)5) << "Inbound Connection Successful!";
            } else if (conn_resp == 1) {
                LOG((Severity)2) << "Inbound Connection Failed";
            } else {
                LOG((Severity)1) << "Inbound Connection tragically lost at sea..";
            }
        }
        ImGui::SameLine();
        
        if (ImGui::Button("Configure"))
            ImGui::OpenPopup("Configure?");

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Configure?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("This operation requires an active socket!\n\n");
            ImGui::Separator();
            ImGui::InputText("host", host, IM_ARRAYSIZE(host));
            ImGui::InputText("port (incoming)", char_port, IM_ARRAYSIZE(char_port));
            ImGui::InputText("port (outgoing)", char_port2, IM_ARRAYSIZE(char_port2));
            static bool dont_ask_me_next_time = false;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PopStyleVar();

            if (ImGui::Button("OK", ImVec2(120, 0))) {
                port = atoi(char_port);
                port2 = atoi(char_port2);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            //ImGui::SameLine();
            //if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        
        if (ImGui::Button("Place Order")) {
            ImGui::OpenPopup("Place Order?");
            //LOG((Severity)5) << "Executing Function3";
        }
        if (ImGui::BeginPopupModal("Place Order?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            //ImGui::Text("Test..\n\n");
            //ImGui::LabelText("label", "Value"); {
                // Using the _simplified_ one-liner Combo() api here
                // See "Combo" section for examples of how to use the more complete BeginCombo()/EndCombo() api.
            //const char* items[] = { "UAL", "DAL", "AAL", "LUV", "XOM", "CVX", "COP", "DVN", "BP", "USO", "DBO", "OIH", "IEZ", "XLE", "VDE", "FENY", "ICLN", "PBW", "LMT", "NOC", "MRO", "OXY", "FANG", "PDX", "NTR", "MOS", "CF", "ICL" };
            
            const char* items[] = {"UAL","DAL","AAL","LUV","RCL","CCL","XOM","CVX","COP","DVN","EOG","PSX","VLO","BP","KMI","HES","USO","DBO","UCO","SCO","OIH","IEZ","IXC","IYE","XLE","VDE","XOP","FENY","QCLN","ICLN","PBW","LMT","NOC","VIAC","WWE","LYV","MSGS","MRO","OXY","FANG","PXD","NTR","MOS","CL","ICL"};
            
            static int item_current = 0;
            ImGui::Combo("ticker", &item_current, items, IM_ARRAYSIZE(items));
            ImGui::SameLine(); HelpMarker(
                "Refer to the \"Combo\" section below for an explanation of the full BeginCombo/EndCombo API, "
                "and demonstration of various flags.\n");
            enum Element { Element_Buy, Element_Sell, Element_COUNT };
            static int elem = Element_Buy;
            const char* elems_names[Element_COUNT] = { "BUY", "SELL" };
            const char* elem_name = (elem >= 0 && elem < Element_COUNT) ? elems_names[elem] : "Unknown";
            static double d0 = 99.9900;
            static int i0 = 123;
            ImGui::Separator();
            if (ImGui::Button("Auto", ImVec2(240, 0))) {
                if (PnL[items[item_current]].pos > 0) {
                    elem = Element_Sell;
                } else {
                    elem = Element_Buy;
                }
                d0 = 0.00;
                i0 = abs(PnL[items[item_current]].pos);
            }
            //ImGui::Separator();
            ImGui::SliderInt("action", &elem, 0, Element_COUNT - 1, elem_name);
            ImGui::SameLine(); HelpMarker("Using the format string parameter to display a name instead of the underlying integer.");
            ImGui::InputDouble("price", &d0, 0.01f, 1.0f, "%.2f");
            ImGui::InputInt("size", &i0);
            ImGui::Separator();
            if (ImGui::Button("Confirm", ImVec2(120, 0))) {
                std::stringstream ss;
                //ss.str ("Example string");
                ss << 0 << ":" << items[item_current] << ":" << elems_names[elem] << ":" << i0 << ":" << d0 << ":";
                //std::cout << 0 << ":" << items[item_current] << ":" << elems_names[elem] << ":" << i0 << ":" << d0 << ":";
                std::string s = ss.str();
                LOG((Severity)6) << s;
                //std::cout << ss << std::endl;
                SocketWrite(s);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        
        if (ImGui::Button("F4")) {
            LOG((Severity)5) << "Executing Function4";
        }
        ImGui::Separator();
        //if (open_action != -1)
          //  ImGui::SetNextItemOpen(open_action != 0);*/
        //if (ImGui::TreeNode("Portfolio"))
        if (ImGui::CollapsingHeader("Portfolio"))
        {
            //static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ColumnsWidthFixed | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable;
            static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY;
            static int row_bg_type = 0;
            static int row_bg_target = 1;
            static int cell_bg_type = 0;

            ImGui::Separator();
            ImGui::BulletText("Account balance: $%g\n",netLiq);
            ImGui::BulletText("Available funds: $%g\n\n",fundsAvailable);

            ImVec2 size = ImVec2(0, 400);
            if (ImGui::BeginTable("##Table", 5, flags, size))
            {
                ImGui::TableSetupScrollFreeze(0, 1); // Make top row always visible
                ImGui::TableSetupColumn("Ticker", ImGuiTableColumnFlags_None);
                ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_None);
                ImGui::TableSetupColumn("Daily P&L", ImGuiTableColumnFlags_None);
                ImGui::TableSetupColumn("Unrealized", ImGuiTableColumnFlags_None);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_None);
                //ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_None);//NoSort | ImGuiTableColumnFlags_WidthFixed, -1.0f, MyItemColumnID_Action);
                ImGui::TableHeadersRow();
                
                for (int row = 0; row < 46; row++)
                {
                    ImGui::TableNextRow();

                    // Demonstrate setting a row background color with 'ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBgX, ...)'
                    // We use a transparent color so we can see the one behind in case our target is RowBg1 and RowBg0 was already targeted by the ImGuiTableFlags_RowBg flag.
                    if (row_bg_type != 0) {
                        ImU32 row_bg_color = ImGui::GetColorU32(row_bg_type == 1 ? ImVec4(0.7f, 0.3f, 0.3f, 0.65f) : ImVec4(0.2f + row * 0.1f, 0.2f, 0.2f, 0.65f)); // Flat or Gradient?
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0 + row_bg_target, row_bg_color);
                    } else {
                        if (PnL[R2S[row]].PnL_unrealized < 0) {
                            // red
                            ImU32 row_bg_color = ImGui::GetColorU32(ImVec4(1.0f, 0.0f, 0.0f, 0.3f));
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0 + row_bg_target, row_bg_color);
                        } else if (PnL[R2S[row]].PnL_unrealized > 0) {
                            ImU32 row_bg_color = ImGui::GetColorU32(ImVec4(0.0f, 0.68f, 0.0f, 0.3f));
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0 + row_bg_target, row_bg_color);
                        }
                    }
                    
                    // Fill cells
                    for (int column = 0; column < 5; column++) {
                        ImGui::TableSetColumnIndex(column);
                        
                        if (row < 43) {
                            double reformated;
                            switch (column) {
                                case 0:
                                    ImGui::Text("%s",R2S[row].c_str());
                                    break;
                                case 1:
                                    ImGui::Text("%d",PnL[R2S[row]].pos);
                                    break;
                                case 2:
                                    ImGui::Text("$ %0.2f",PnL[R2S[row]].PnL_daily);
                                    break;
                                case 3:
                                    ImGui::Text("$ %0.2f",PnL[R2S[row]].PnL_unrealized);
                                    break;
                                case 4:
                                    ImGui::Text("$ %0.2f",PnL[R2S[row]].value);
                                    break;
                            }
                        }
                        // Change background of Cells B1->C2
                        // Demonstrate setting a cell background color with 'ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ...)'
                        // (the CellBg color will be blended over the RowBg and ColumnBg colors)
                        // We can also pass a column number as a third parameter to TableSetBgColor() and do this outside the column loop.
                        if (row >= 1 && row <= 2 && column >= 1 && column <= 2 && cell_bg_type == 1)
                        {
                            //cmyk(100%, 0%, 100%, 33%) 1.0f, 0.0f, 1.0f, 0.33f
                            //ImU32 cell_bg_color = ImGui::GetColorU32(ImVec4(0.3f, 0.3f, 0.7f, 0.65f));
                            ImU32 cell_bg_color = ImGui::GetColorU32(ImVec4(0.0f, 0.68f, 0.0f, 0.3f));
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color);
                        }
                        
                        if (row == 43 && column >= 2 && column <= 4) {
                            //std::vector<double> sums = {0,0,0};
                            double sum = 0;
                            for (int i = 0; i < 28; i++) {
                                switch (column) {
                                    case 2:
                                        sum += PnL[R2S[i]].PnL_daily;
                                        break;
                                    case 3:
                                        sum += PnL[R2S[i]].PnL_unrealized;
                                        break;
                                    case 4:
                                        sum += PnL[R2S[i]].value;
                                        break;
                                }
                            }
                            liq_t pv_t;
                            pv_t.p_val = sum;
                            epoch(pv_t.t);
                            pv_c.push_back(pv_t);
                            ImGui::Text("$ %0.2f",sum);
                        }
                    }
                }
                ImGui::EndTable();
            }
        }
        
        ImGui::Separator();
        
        if (ImGui::Button("Clear")) writer.logs.clear();
        ImGui::SameLine();
        filter.Draw("Filter",-50);
        ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

        for (unsigned int i = 0; i < writer.logs.size(); ++i) {
            if (filter.PassFilter(writer.logs[i].second.c_str())) {
                ImGui::PushStyleColor(ImGuiCol_Text, colors[writer.logs[i].first]);
                ImGui::TextUnformatted(writer.logs[i].second.c_str());
                ImGui::PopStyleColor();
            }
        }
        /*if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        */
        ImGui::EndChild();
        ImGui::End();
        if (!open) quit();
    }
    bool open = true;
};

int main() {
    Calculon app;
    app.run();
    return 0;
}
