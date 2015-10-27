#include <iostream>
#include <map>
#include <fstream>
#include <cstdio>
#include <vector>
#include <string>
#include <sstream>
#include <cctype>
#include <stdexcept>
#include <cstdint>
#include <iomanip>
#include <utility>
#include <cstring>

struct __attribute__ ((__packed__)) mode_rec{
	uint32_t signature; /**< Signature, OR'd with number of mode record shifted right by one bit and LAST_RECORD flag*/
	uint8_t xres, yres; /**< Horizontal and vertical resolution, shifted right by 3 bits*/
	uint16_t framerate; /**< Framerate, technically, frame period, in milliseconds*/
	uint32_t milestone; /**< Time milestone, i.e. the time when we are to proceed to next mode in sequence*/
	uint8_t q_table; /**< Number of quantization table*/
	uint8_t flags; /**< Flags, bit 0 means Prediction activity, bits 2:1 - JPEG file destination*/
	uint16_t checksum; /**< Checksum, Fletcher16*/
};

/** @brief Sequence's first page address*/
#define MODE_ADDR 0x0020
/** @brief Size of Sequence record, bytes*/
#define MODESIZE 16
/** @brief Number of Modes in Sequence*/
#define MODE_NUM 4
/** @brief Mode record signature*/
#define MODE_SIGN 0x97CA53E0

using namespace std;

string tolower(string& in){
    string ret(in);
    for(char& i : ret) i = tolower(i);
    return ret;
}

uint16_t Fletcher16( const uint8_t *data, uint32_t len ){
        uint16_t sum1 = 0xff, sum2 = 0xff;
        while (len){
                uint32_t tlen = len > 20 ? 20 : len;
                len -= tlen;
                do{
                        sum2 += sum1 += *data++;
                }while (--tlen);
                sum1 = (sum1 & 0xff) + (sum1 >> 8);
                sum2 = (sum2 & 0xff) + (sum2 >> 8);
        }
        /* Second reduction step to reduce sums to 8 bits */
        sum1 = (sum1 & 0xff) + (sum1 >> 8);
        sum2 = (sum2 & 0xff) + (sum2 >> 8);
        return sum2 << 8 | sum1;
}



int main(int argc, char** argv){
    if(argc < 3){
        cout << "Plan conversion utility.\n\tUsage: " << argv[0] << " <infile> <outfile>" << endl;
        return 0;
    }

    try{

    ifstream infile(argv[1]);
    if(!infile.is_open()) throw runtime_error("Can't open infile");
    stringstream config;

    config << infile.rdbuf();

    infile.close();

    map<uint8_t, string> modes;
    mode_rec bmodes[MODE_NUM];
    memset(bmodes, 0, sizeof(mode_rec)*MODE_NUM);

//1. strip comments
    string cnf = config.str();
    size_t pos = 0;
    while(pos != string::npos){
        pos = cnf.find_first_of('#');
        if(pos != string::npos){
            size_t pos2 = cnf.find_first_of('\n',pos);
            if(pos2 == string::npos) pos2 = cnf.size();
            cnf.erase(pos, pos2-pos+1);
        }
    }
    config.str(cnf);
    cnf.clear();
//2. separate into modes
    config >> cnf;
    while(!config.eof()){
        if(tolower(cnf) == "end") break;
        else if(tolower(cnf) != "mode") throw runtime_error("Found \"" + cnf +  "\" instead of \"Mode\"");
        int mn;
        config >> mn;
        if(mn >= MODE_NUM) throw runtime_error("Bad mode number, must be in range 0 - " + to_string(MODE_NUM-1) + ", but got " + to_string(mn));
        config >> cnf;
        if(cnf[0] != '{') throw runtime_error("Can't find mode delimiter (mode #" + to_string(mn) + ")");
        getline(config, cnf, '}');
        modes[mn] = cnf;
        config >> cnf >> cnf;
    }
//3. parse individual modes
    for(unsigned int i = 0; i < modes.size(); i++){
        map<string, string> mode;
        stringstream smode(modes[i]);
        string key, data;
        int idata;
        float fdata;
        char cdata;

//3.1 divide
        while(!smode.eof()){
            smode >> skipws >> key;
            getline(smode,data);
            if(data.find('{') != string::npos){
                getline(smode, data, '}');
                smode >> cdata;
            }
            mode[tolower(key)] = tolower(data);
        }
//3.2 interpret
        mode_rec curmode;
        curmode.signature = MODE_SIGN | (i<<1) | (i == modes.size()-1);
        curmode.xres = stoi(mode["x"]) >> 3;
        curmode.yres = stoi(mode["y"]) >> 3;

        stringstream s1(mode["f"]);
        s1 >> fdata >> data;
        if(data == "fps") curmode.framerate = 1000.0/fdata;
        else if(data == "ms") curmode.framerate = fdata;
        else if(data == "s") curmode.framerate = fdata*1000.0;
        else throw runtime_error("Bad framerate/frame period units in mode #" + to_string(i));

        if(i != modes.size() - 1){
            stringstream s2(mode["t"]);
            s2 >> idata >> data;
            if(data == "s") curmode.milestone = idata << 15;
            else if(data == "min") curmode.milestone = (idata*60) << 15;
            else if(data == "h") curmode.milestone = (idata*3600) << 15;
            else throw runtime_error("Bad plan time units in mode #" + to_string(i));
        } else curmode.milestone = 0;
        curmode.q_table = stoi(mode["q"]);

        if(isdigit(mode["flags"][0])) curmode.flags = stoi(mode["flags"]);
        else{
            uint8_t flags = 0;
            stringstream s3(mode["flags"]);
            map<string, string> fl;
            while(!s3.eof()){
                s3 >> key >> data;
                fl[tolower(key)] = tolower(data);
            }

            uint8_t pf = stoi(fl["p"]);
            if(pf >= 8) throw runtime_error("Too high Threshold value in mode #" + to_string(i));
            flags |= pf&0x7;

            //more...

            curmode.flags = flags;
        }

//        curmode.checksum = Fletcher16((uint8_t *)&curmode,MODESIZE-2);

        bmodes[i] = curmode;
    }
    //integrate times in plans
    uint32_t cur_time = 0;
    for(unsigned int i = 0; i < modes.size(); i++){
        cur_time += bmodes[i].milestone;
        bmodes[i].milestone = cur_time;
        bmodes[i].checksum = Fletcher16((uint8_t *)&bmodes[i],MODESIZE-2);
    }

    FILE* out = fopen(argv[2], "wb");
    if(!out) throw runtime_error("Can't open outfile");
    uint8_t nullpage[32] = { 0 };
    fwrite(nullpage,1,32,out);
    fwrite(bmodes, MODESIZE, MODE_NUM, out);
    fclose(out);

    } catch (runtime_error& e){
        cout << "Error: " << e.what() << endl;
    }

    return 0;
}
