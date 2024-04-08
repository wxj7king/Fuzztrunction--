#include "pin.H"
#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <set>
#include <algorithm>
#include <random>
#include <sys/ipc.h>
#include <sys/shm.h>

using std::cerr;
using std::cout;
using std::dec;
using std::endl;
using std::flush;
using std::hex;

#define MAX_MUT_PPS 5
#define SHM_NAME "/FTMM_SHM"
#define MAX_MASKS_SIZE 1024 * 16
typedef struct patch_point{
    ADDRINT addr;
    size_t shm_offset;
} Patchpoint;
typedef std::vector<Patchpoint> Patchpoints;
typedef struct shm_para{
    key_t key;
    int shm_id;
    unsigned char *shm_base_ptr;
    unsigned char *shm_base_ptr_old;
    size_t size_in_bytes;
}Shm_para;
typedef struct masks{
    uint32_t num_iter;
    uint64_t addr;
    uint64_t cur_iter;
    unsigned char masks[MAX_MASKS_SIZE];
}Masks;

std::set<std::string> lib_blacklist;
Patchpoints patch_points;
Shm_para shm_para;
BOOL detach_flag;
KNOB<std::string> KnobNewOff(KNOB_MODE_WRITEONCE, "pintool", "offset", "0", "specify addrs of instructions");
KNOB<std::string> KnobNewNum(KNOB_MODE_WRITEONCE, "pintool", "num", "512", "specify how many pps will be mutated");
KNOB<std::string> KnobNewNumThread(KNOB_MODE_WRITEONCE, "pintool", "num_t", "512", "specify how many pps will be mutated");

VOID ApplyMutation(ADDRINT Ip, REG reg, UINT32 reg_size, UINT32 offset, ADDRINT addr, CONTEXT *ctx){
    // use flip_idxes[cur_pps] as the number of current steps
    if (reg_size == 0) return;
    Masks *masks = (Masks *)shm_para.shm_base_ptr;
    masks += offset;
    if (masks->cur_iter > masks->num_iter) return;
    assert(addr == masks->addr);

    UINT8 *reg_val = (UINT8 *)malloc(reg_size);
    PIN_GetContextRegval(ctx, reg, reg_val);
    //printf("instruction@0x%lx, register %s, original value=%ld ", Ip, REG_StringShort(reg).c_str(), *(ADDRINT*)reg_val);
    // apply mask
    for (size_t i = 0; i < reg_size; i++){
        reg_val[i] ^= masks->masks[masks->cur_iter * reg_size + i];
    }
    masks->cur_iter++;
    //printf("mutated value=%ld\n", *(ADDRINT*)reg_val);
    PIN_SetContextRegval(ctx, reg, reg_val);
    free(reg_val);
    return;
}

VOID InstrumentIns(INS ins, ADDRINT baseAddr)
{
    //xed_iclass_enum_t ins_opcode = (xed_iclass_enum_t)INS_Opcode(ins);
    //printf("instruction@%p: %s\n", (void *)INS_Address(ins), INS_Disassemble(ins).c_str());
    ADDRINT addr_offset = (INS_Address(ins) - baseAddr);
    auto it = std::find_if(patch_points.begin(), patch_points.end(), [=](const Patchpoint& pp){return pp.addr == addr_offset;});
    if (it == patch_points.end()) return;
    Patchpoint pp = *it;
    patch_points.erase(it);
    if (patch_points.empty()) detach_flag = true;

    //AFUNPTR mut_func = NULL;
    IPOINT ipoint;
    REG reg2mut = REG_INVALID();
    REGSET regsetIn, regsetOut;
    UINT32 reg_size = 0;

    if (INS_IsMemoryRead(ins)){
        if (!INS_OperandIsReg(ins, 0)) return;
        reg2mut = INS_OperandReg(ins, 0);
        //printf("data read instruction@%p, %s, %s\n", (void *)INS_Address(ins), INS_Disassemble(ins).c_str(), REG_StringShort(reg2mut).c_str());
        ipoint = IPOINT_AFTER;
    }

    if (INS_IsMemoryWrite(ins)){
        if (!INS_OperandIsReg(ins, 1)) return;
        reg2mut = INS_OperandReg(ins, 1);
        //printf("data write instruction@%p, %s, %s\n", (void *)INS_Address(ins), INS_Disassemble(ins).c_str(), REG_StringShort(reg2mut).c_str());
        ipoint = IPOINT_BEFORE;
    }

    REGSET_Insert(regsetIn, reg2mut);
    REGSET_Insert(regsetIn, REG_FullRegName(reg2mut));
    REGSET_Insert(regsetOut, reg2mut);
    REGSET_Insert(regsetOut, REG_FullRegName(reg2mut));
    reg_size = REG_Size(reg2mut);

    printf("instruction@%p, %s, end flag=%d, ipoint=%d\n", (void *)pp.addr, INS_Disassemble(ins).c_str(), detach_flag, ipoint);

    INS_InsertCall(ins, ipoint, (AFUNPTR)ApplyMutation,
                IARG_INST_PTR,// application IP
                IARG_UINT32, reg2mut,
                IARG_UINT32, reg_size,
                IARG_UINT32, pp.shm_offset,
                IARG_ADDRINT, pp.addr,
                IARG_PARTIAL_CONTEXT, &regsetIn, &regsetOut,
                IARG_END);
         

}

const char* StripPath(const char* path)
{
    const char* file = strrchr(path, '/');
    if (file)
        return file + 1;
    else
        return path;
}

// XXX: cannot use routine because the changed register value will not saved
VOID InstrumentTrace(TRACE trace, VOID *v){
    if (detach_flag) return;
    ADDRINT baseAddr = 0;
    RTN rtn = TRACE_Rtn(trace);
    std::string img_name;
    if (RTN_Valid(rtn)){
        IMG img = SEC_Img(RTN_Sec(rtn));
        img_name = StripPath(IMG_Name(img).c_str());
        if (lib_blacklist.find(img_name) != lib_blacklist.end()) return;
        baseAddr = IMG_LowAddress(img);
    }
    else return;
    //printf("In %s\n", img_name.c_str());

    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)){
            InstrumentIns(ins, baseAddr);
        }

    }
}

void Detatched(VOID *v){std::cerr << endl << "Detached from pintool!" << endl;}

void Fini(INT32 code, void *v){
	// printf("read counts: %ld\n", read_count);
    // printf("write counts: %ld\n", write_count);
	// printf("read number of insts: %ld\n", instmap.size());
    shmdt(shm_para.shm_base_ptr_old);

    return;
}

INT32 Usage()
{
    // cerr << "This Pintool counts the number of times a routine is executed" << endl;
    // cerr << "and the number of instructions executed in a routine" << endl;
    cerr << endl << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}

std::vector<std::string> get_tokens(std::string args, std::string del){
    size_t pos_s = 0;
    size_t pos_e;
    std::string token;
    std::vector<std::string> vec;
    while((pos_e = args.find(del, pos_s)) != std::string::npos){
        token = args.substr(pos_s, pos_e - pos_s);
        pos_s = pos_e + del.length();
        vec.push_back(token);
    }
    vec.push_back(args.substr(pos_s));
    return vec;
}

BOOL Init(){
    size_t offset = std::stoul(KnobNewOff.Value());
    size_t pps_num = std::stoul(KnobNewNum.Value());
    size_t num_thread = std::stoul(KnobNewNumThread.Value());
    ASSERT(pps_num <= MAX_MUT_PPS, "number of pps invalid!");
    // ASSERT(offset < NUM_THREAD);

    shm_para.size_in_bytes = num_thread * MAX_MUT_PPS * sizeof(Masks);
    shm_para.key = ftok(SHM_NAME, 'A');
    shm_para.shm_id = shmget(shm_para.key, shm_para.size_in_bytes, 0666);
    if (shm_para.shm_id == -1){
        std::cerr << "shared memory get failed!\n";
        return false;
    }
    shm_para.shm_base_ptr_old = (unsigned char *)shmat(shm_para.shm_id, NULL, 0);
    if (shm_para.shm_base_ptr_old == (void *)-1){
        std::cerr << "shmat() failed!\n";
        return false;
    }
    shm_para.shm_base_ptr = shm_para.shm_base_ptr_old + (offset * MAX_MUT_PPS * sizeof(Masks));
    //printf("shared memory opened successfully! Size: %ld Bytes\n", shm_para.size_in_bytes);

    lib_blacklist.insert("ld-linux-x86-64.so.2");
    lib_blacklist.insert("[vdso]");
    lib_blacklist.insert("libc.so.6");

    Masks *msk_ptr_base = (Masks *)shm_para.shm_base_ptr;
    for (size_t i = 0; i < pps_num; i++)
    {
        Masks *msk_ptr = msk_ptr_base + i;
        Patchpoint pp;
        pp.shm_offset = i;
        pp.addr = msk_ptr->addr;
        patch_points.push_back(pp);
        //printf("pp: %p\n", (void *)pp.addr);
    }
    

    return true;
}

int main(INT32 argc, CHAR* argv[])
{   
    if (PIN_Init(argc, argv)) return Usage();
    if (!Init()) return Usage();
    //PIN_SetSyntaxATT();
    //INS_AddInstrumentFunction(InstrumentIns, 0);
    TRACE_AddInstrumentFunction(InstrumentTrace, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_AddDetachFunction(Detatched, 0);
    PIN_StartProgram();
    return 0;
}