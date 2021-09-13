//
//  amfid.m
//  reton
//
//  Created by Luca on 18.02.21.
//
//

#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach-o/nlist.h>
#include <mach-o/getsect.h>
#include <CoreFoundation/CoreFoundation.h>
#include <lib/tq/tq_common_p.h>
#include <lib/tq/utils.h>


void task_read(mach_port_t task, uintptr_t addr, void *data, size_t len){
    kern_return_t kr;
    vm_size_t outsize = len;

    kr = vm_read_overwrite(task, addr, len, (vm_address_t)data, &outsize);
    if (kr != KERN_SUCCESS) {
        util_error("%s: kr %d: %s", __func__, kr, mach_error_string(kr));
    }
}

void task_write(mach_port_t task, uintptr_t addr, void *data, size_t len){
    kern_return_t kr;
    mach_msg_type_number_t size = (mach_msg_type_number_t)len;

    kr = vm_write(task, addr, (vm_offset_t)data, size);
    if (kr != KERN_SUCCESS) {
        util_error("%s: kr %d: %s", __func__, kr, mach_error_string(kr));
    }
}

uint64_t task_read64(mach_port_t task, uintptr_t addr){
    uint64_t v = 0;
    task_read(task, addr, &v, sizeof(v));
    return v;
}

void task_write64(mach_port_t task, uintptr_t addr, uint64_t v){
    task_write(task, addr, &v, sizeof(v));
}

uint64_t task_alloc(mach_port_t task, size_t len){
    vm_address_t return_addr = 0;
    vm_allocate(task, (vm_address_t*)&return_addr, len, VM_FLAGS_ANYWHERE);
    return return_addr;
}

void task_dealloc(mach_port_t task, uint64_t addr, size_t len){
    vm_deallocate(task, addr, len);
}

typedef CF_OPTIONS(uint32_t, SecCSFlags) {
    kSecCSDefaultFlags = 0,                    /* no particular flags (default behavior) */
    kSecCSConsiderExpiration = 1 << 31,        /* consider expired certificates invalid */
};

typedef void *SecStaticCodeRef;
OSStatus SecStaticCodeCreateWithPathAndAttributes(CFURLRef path, SecCSFlags flags, CFDictionaryRef attributes, SecStaticCodeRef  _Nullable *staticCode);
OSStatus SecCodeCopySigningInformation(SecStaticCodeRef code, SecCSFlags flags, CFDictionaryRef  _Nullable *information);
CFStringRef (*_SecCopyErrorMessageString)(OSStatus status, void * __nullable reserved) = NULL;

enum cdHashType {
    cdHashTypeSHA1 = 1,
    cdHashTypeSHA256 = 2
};

static const char *cdHashName[3] = { NULL, "SHA1", "SHA256" };

static enum cdHashType requiredHash = cdHashTypeSHA256;
#define TRUST_CDHASH_LEN (20)

const void *CFArrayGetValueAtIndex_prevenOverFlow(CFArrayRef theArray, CFIndex idx){
    CFIndex arrCnt = CFArrayGetCount(theArray);
    if(idx >= arrCnt){
        idx = arrCnt - 1;
    }
    return CFArrayGetValueAtIndex(theArray, idx);
}

bool calc_cdhash(const char *filepath, uint8_t outcdhash[TRUST_CDHASH_LEN]){
    SecStaticCodeRef staticCode = NULL;

    CFStringRef cfstr_path = CFStringCreateWithCString(kCFAllocatorDefault, filepath, kCFStringEncodingUTF8);
    CFURLRef cfurl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, cfstr_path, kCFURLPOSIXPathStyle, false);
    CFRelease(cfstr_path);
    OSStatus result = SecStaticCodeCreateWithPathAndAttributes(cfurl, kSecCSDefaultFlags, NULL, &staticCode);
    CFRelease(cfurl);
    if (result != 0) {
        if (_SecCopyErrorMessageString != NULL) {
            CFStringRef error = _SecCopyErrorMessageString(result, NULL);

            util_error("Unable to generate cdhash for %s: %s", filepath, CFStringGetCStringPtr(error, kCFStringEncodingUTF8));
            CFRelease(error);
        } else {
            util_error("Unable to generate cdhash for %s: %d", filepath, result);
        }
        return false;
    }

    CFDictionaryRef signinginfo;
    result = SecCodeCopySigningInformation(staticCode, kSecCSDefaultFlags, &signinginfo);
    CFRelease(staticCode);
    if (result != 0) {
        util_error("Unable to copy cdhash info for %s", filepath);
        return false;
    }

    CFArrayRef cdhashes = CFDictionaryGetValue(signinginfo, CFSTR("cdhashes"));
    CFArrayRef algos = CFDictionaryGetValue(signinginfo, CFSTR("digest-algorithms"));
    int algoIndex = -1;
    CFNumberRef nn = CFArrayGetValueAtIndex_prevenOverFlow(algos, requiredHash);
    if(nn){
        CFNumberGetValue(nn, kCFNumberIntType, &algoIndex);
    }

    //(printf)("cdhashesCnt: %d\n", CFArrayGetCount(cdhashes));
    //(printf)("algosCnt: %d\n", CFArrayGetCount(algos));

    CFDataRef cdhash = NULL;
    if (cdhashes == NULL) {
        util_error("%s: no cdhashes", filepath);
    } else if (algos == NULL) {
        util_error("%s: no algos", filepath);
    } else if (algoIndex == -1) {
        util_error("%s: does not have %s hash", cdHashName[requiredHash], filepath);
    } else {
        cdhash = CFArrayGetValueAtIndex_prevenOverFlow(cdhashes, requiredHash);
        if (cdhash == NULL) {
            util_error("%s: missing %s cdhash entry", filepath, cdHashName[requiredHash]);
        }
    }
    if(cdhash == NULL){
        CFRelease(signinginfo);
        return false;
    }

    //(printf)("cdhash len: %d\n", CFDataGetLength(cdhash));
    memcpy(outcdhash, CFDataGetBytePtr(cdhash), TRUST_CDHASH_LEN);
    CFRelease(signinginfo);
    return true;
}

void *Build_ValidateSignature_dic(uint8_t *input_cdHash, size_t *out_size, uint64_t shadowp){
    // Build a self-contained, remote-address-adapted CFDictionary instance

    CFDataRef _cfhash_cfdata = CFDataCreate(kCFAllocatorDefault, input_cdHash, TRUST_CDHASH_LEN);
    void *cfhash_cfdata = (void*)_cfhash_cfdata;
    const char *iomatch_key = "CdHash"; // kMISValidationInfoCdHash

    size_t key_len = strlen(iomatch_key) + 0x11;
    key_len = (~0xF) & (key_len + 0xF);
    size_t value_len = 0x60; // size of self-contained CFData instance
    value_len = (~0xF) & (value_len + 0xF);
    size_t total_len = key_len + value_len + 0x40;

    *out_size = total_len;
    void *writep = calloc(1, total_len);

    char *realCFString = (char*)CFStringCreateWithCString(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", kCFStringEncodingUTF8);
    const void *keys[] = { realCFString };
    const void *values[] = { cfhash_cfdata };
    char *realCFDic = (char*)CFDictionaryCreate(0, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRetain(realCFDic); // Pump in some extra lifes [x4]
    CFRetain(realCFDic);
    CFRetain(realCFDic);
    CFRetain(realCFDic);
    memcpy(writep, realCFDic, 0x40);

    writep = writep + total_len - value_len;
    shadowp = shadowp + total_len - value_len;
    uint64_t value = shadowp;
    memcpy(writep, cfhash_cfdata, 0x60);
    CFRelease(cfhash_cfdata);

    writep -= key_len;
    shadowp -= key_len;
    uint64_t key = shadowp;
    *(uint64_t*)(writep) = *(uint64_t*)realCFString;
    *(uint64_t*)(writep + 8) = *(uint64_t*)(realCFString + 8);
    *(uint8_t*)(writep + 16) = strlen(iomatch_key);
    memcpy(writep + 17, iomatch_key, strlen(iomatch_key) + 1);

    writep -= 0x40;
    shadowp -= 0x40;
    *(uint64_t*)(writep + 0x10) = 0x41414141; // Key
    *(uint64_t*)(writep + 0x18) = 0x42424242; // Value
    *(uint64_t*)(writep + 0x20) = key; // 0x43434343
    *(uint64_t*)(writep + 0x28) = value; // 0x44444444
    *(uint64_t*)(writep + 0x30) = 0; // 0x45454545
    *(uint64_t*)(writep + 0x38) = 0; // 0x46464646

    CFRelease(realCFDic);
    CFRelease(realCFDic);
    CFRelease(realCFDic);
    CFRelease(realCFDic);
    CFRelease(realCFDic);
    CFRelease(realCFString);

    return writep;
}

#pragma pack(push, 4)
typedef struct {
    mach_msg_header_t Head;
    mach_msg_body_t msgh_body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t NDR;
} exception_raise_request; // Bits needed

typedef struct {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    kern_return_t RetCode;
} exception_raise_reply;
#pragma pack(pop)

static mach_port_t amfid_exception_port;
uint64_t amfid_alloc_page = 0;
uint64_t amfid_cdhash_off = 0;
uint64_t amfid_dict_isa = 0;

static void reply_thread_exception(exception_raise_request *req){
    kern_return_t kr;
    exception_raise_reply reply = {};
    mach_msg_size_t send_size = sizeof(reply);
    mach_msg_size_t recv_size = 0;

    reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(req->Head.msgh_bits), 0);
    reply.Head.msgh_size = sizeof(reply);
    reply.Head.msgh_remote_port = req->Head.msgh_remote_port;
    reply.Head.msgh_local_port = MACH_PORT_NULL;
    reply.Head.msgh_id = req->Head.msgh_id + 100;

    reply.NDR = req->NDR;
    reply.RetCode = KERN_SUCCESS;

    kr = mach_msg(&reply.Head, MACH_SEND_MSG,
            send_size, recv_size,
            MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    fail_if(kr != KERN_SUCCESS, "failed to reply %s", mach_error_string(kr));
}

static void *amfid_exception_thread(void *args){
    kern_return_t kr;
    arm_thread_state64_t ts;
    mach_msg_type_number_t tscount;
    mach_msg_size_t send_size = 0;
    mach_msg_size_t recv_size = 0x1000;
    mach_msg_header_t *msg = malloc(recv_size);
#if __arm64e__
    uintptr_t callee_lr = 0; // BRAA 0x41414141
    uintptr_t signed_pc = 0; // return to
#endif

    for(;;) {
        //util_info("calling mach_msg to receive exception message from amfid");
        kr = mach_msg(msg, MACH_RCV_MSG,
                send_size, recv_size,
                amfid_exception_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            util_info("error receiving on exception port: %s", mach_error_string(kr));
            continue;
        }

        exception_raise_request* req = (exception_raise_request*)msg;

        mach_port_t thread_port = req->thread.name;
        mach_port_t task_port = req->task.name;

        tscount = ARM_THREAD_STATE64_COUNT;
        kr = thread_get_state(thread_port, ARM_THREAD_STATE64, (thread_state_t)&ts, &tscount);
        if (kr != KERN_SUCCESS){
            util_error("error getting thread state: %s", mach_error_string(kr));
            continue;
        }

        uintptr_t pc;
#if __arm64e__
        pc = (uintptr_t)ts.__opaque_pc & 0x0000007fffffffff;
#else
        pc = ts.__pc;
#endif

#if __arm64e__
        // first exceptioin
        if (callee_lr == 0) {
            util_info("first time: pc %p, lr %p", ts.__opaque_pc, ts.__opaque_lr);
            fail_if(pc != 0x41414141, "first exception?");
            callee_lr = (uintptr_t)ts.__opaque_lr & 0x0000007fffffffff;

            void *userspace_PAC_hack(mach_port_t target_thread, void *pc);
            signed_pc = (uintptr_t)userspace_PAC_hack(thread_port, (void *)callee_lr);
            util_info("signed pc %#lx", signed_pc);
        }
#endif

        // get the filename pointed to by X22
        char filepath[PATH_MAX];
        task_read(task_port, ts.__x[22], filepath, sizeof(filepath));
        util_info("amfid request: %s", filepath);

        uint32_t dict_off = 0x00;
        uint8_t cdhash[TRUST_CDHASH_LEN];
        bool ok = calc_cdhash(filepath, cdhash);
        if (ok) {
            if (amfid_alloc_page == 0) {
                // Allocate a page of memory in amfid, where we stored cfdic for bypass signature valid
                amfid_alloc_page = task_alloc(task_port, g_exp.pagesize);
                util_info("amfid_alloc_page: 0x%llx", amfid_alloc_page);

                size_t out_size = 0;
                char *fakedic = Build_ValidateSignature_dic(cdhash, &out_size, amfid_alloc_page + dict_off);
                util_hexprint_width(fakedic, out_size, 8, "fake cdhash dict");
                task_write(task_port, amfid_alloc_page + dict_off, fakedic, (uint32_t)out_size);
                amfid_cdhash_off = amfid_alloc_page + dict_off + 0x90; // To update cdhash in the same cfdic
                amfid_dict_isa = *(uint64_t*)(fakedic); // To keep dic away from being release
                free(fakedic);
            }
            task_write(task_port, amfid_cdhash_off, cdhash, sizeof(cdhash));
            task_write64(task_port, amfid_alloc_page + dict_off, amfid_dict_isa);
        }

        task_write64(task_port, ts.__x[2], amfid_alloc_page + dict_off);
        ts.__x[0] = 0; // MISValidateSignatureAndCopyInfo success
#if __arm64e__ 
        ts.__opaque_pc = (void *)signed_pc;
#else
        ts.__pc = ts.__lr; // ret
#endif

        // set the new thread state:
        kr = thread_set_state(thread_port, ARM_THREAD_STATE64, (thread_state_t)&ts, ARM_THREAD_STATE64_COUNT);

        reply_thread_exception(req);

        mach_port_deallocate(mach_task_self(), thread_port);
        mach_port_deallocate(mach_task_self(), task_port);
    }
    return NULL;
}

void amfid_set_exception_handler(mach_port_t amfid_task){
    kern_return_t kr;

    // allocate a port to receive exceptions on:
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &amfid_exception_port);
    mach_port_insert_right(mach_task_self(), amfid_exception_port, amfid_exception_port, MACH_MSG_TYPE_MAKE_SEND);

    kr = task_set_exception_ports(amfid_task,
                                  EXC_MASK_ALL,
                                  amfid_exception_port,
                                  EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,  // we want to receive a catch_exception_raise message with the thread port for the crashing thread
                                  ARM_THREAD_STATE64);

    if (kr != KERN_SUCCESS){
        util_error("error setting amfid exception port: %s", mach_error_string(kr));
    } else {
        util_info("set amfid exception port: succeed!");
    }

    // spin up a thread to handle exceptions:
    pthread_t th_exception;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th_exception, &attr, amfid_exception_thread, NULL);
}


bool check_if_its_PAC_device(void){
#if __arm64e__
    return true;
#else
    return false;
#endif
}

uint64_t amfid_OFFSET_MISValidate_symbol = 0; // for redirect code exec

uint64_t find_amfid_OFFSET_MISValidate_symbol(uint8_t *amfid_macho){
    uint32_t MISValidate_symIndex = 0;
    struct mach_header_64 *mh = (struct mach_header_64*)amfid_macho;
    const uint32_t cmd_count = mh->ncmds;
    struct load_command *cmds = (struct load_command*)(mh + 1);
    struct load_command* cmd = cmds;
    for (uint32_t i = 0; i < cmd_count; ++i){
        switch (cmd->cmd) {
            case LC_SYMTAB:{
                struct symtab_command *sym_cmd = (struct symtab_command*)cmd;
                uint32_t symoff = sym_cmd->symoff;
                uint32_t nsyms = sym_cmd->nsyms;
                uint32_t stroff = sym_cmd->stroff;

                for(int i =0;i<nsyms;i++){
                    struct nlist_64 *nn = (void*)((char*)mh+symoff+i*sizeof(struct nlist_64));
                    char *def_str = NULL;
                    if(nn->n_type==0x1){
                        // 0x1 indicates external function
                        def_str = (char*)mh+(uint32_t)nn->n_un.n_strx + stroff;
                        if(!strcmp(def_str, "_MISValidateSignatureAndCopyInfo")){
                            break;
                        }
                    }
                    if(i!=0 && i!=1){ // Two at beginning are local symbols, they don't count
                        MISValidate_symIndex++;
                    }
                }
            }
                break;
        }
        cmd = (struct load_command*)((char*)cmd + cmd->cmdsize);
    }

    if(MISValidate_symIndex == 0){
        fail_info("Error in find_amfid_OFFSET_MISValidate_symbol(): MISValidate_symIndex == 0");
    }

    const struct section_64 *sect_info = NULL;
    if(check_if_its_PAC_device()){
        const char *_segment = "__DATA_CONST", *_segment2 = "__DATA", *_section = "__auth_got";
        // _segment for iOS 13, _segment2 for <= iOS 12
        sect_info = getsectbynamefromheader_64((const struct mach_header_64 *)amfid_macho, _segment, _section);
        if(!sect_info)
            sect_info = getsectbynamefromheader_64((const struct mach_header_64 *)amfid_macho, _segment2, _section);
    }else{
        const char *_segment = "__DATA", *_section = "__la_symbol_ptr";
        sect_info = getsectbynamefromheader_64((const struct mach_header_64 *)amfid_macho, _segment, _section);
    }

    if(!sect_info){
        fail_info("Error in find_amfid_OFFSET_MISValidate_symbol(): if(!sect_info)");
    }

    return sect_info->offset + (MISValidate_symIndex * 0x8);
}

size_t amfid_fsize = 0;
uint8_t *map_file_to_mem(const char *path){

    struct stat fstat = {0};
    stat(path, &fstat);
    amfid_fsize = fstat.st_size;

    int fd = open(path, O_RDONLY);
    uint8_t *mapping_mem = mmap(NULL, mach_vm_round_page(amfid_fsize), PROT_READ, MAP_SHARED, fd, 0);
    fail_if(mapping_mem == MAP_FAILED, "Error in map_file_to_mem()");
    return mapping_mem;
}

uint64_t binary_load_address(mach_port_t tp) {
    kern_return_t err;
    mach_msg_type_number_t region_count = VM_REGION_BASIC_INFO_COUNT_64;
    memory_object_name_t object_name = MACH_PORT_NULL; /* unused */
    mach_vm_size_t target_first_size = 0x1000;
    mach_vm_address_t target_first_addr = 0x0;
    struct vm_region_basic_info_64 region = {0};
    //printf("about to call mach_vm_region\n");
    extern kern_return_t mach_vm_region
    (
     vm_map_t target_task,
     mach_vm_address_t *address,
     mach_vm_size_t *size,
     vm_region_flavor_t flavor,
     vm_region_info_t info,
     mach_msg_type_number_t *infoCnt,
     mach_port_t *object_name
     );
    err = mach_vm_region(tp,
                         &target_first_addr,
                         &target_first_size,
                         VM_REGION_BASIC_INFO_64,
                         (vm_region_info_t)&region,
                         &region_count,
                         &object_name);

    if (err != KERN_SUCCESS) {
        //printf("failed to get the region err: %d\n", err);
        return 0;
    }
    //printf("got base address\n");

    return target_first_addr;
}

void patch_amfid(pid_t amfid_pid){
    uint32_t amfid_task = 0;
    task_for_pid(mach_task_self(), amfid_pid, &amfid_task);
    util_info("amfid_task: 0x%x", amfid_task);
    fail_if(!amfid_task, "task_for_pid failed");

    amfid_set_exception_handler(amfid_task);

    uint8_t *amfid_fdata = map_file_to_mem("/usr/libexec/amfid");
    amfid_OFFSET_MISValidate_symbol = find_amfid_OFFSET_MISValidate_symbol(amfid_fdata);
    util_info("amfid_OFFSET_MISValidate_symbol: 0x%llx", amfid_OFFSET_MISValidate_symbol);
    munmap(amfid_fdata, amfid_fsize);

    uint64_t amfid_base = binary_load_address(amfid_task);
    util_info("amfid_base: 0x%llx", amfid_base);

    kern_return_t kr = vm_protect(amfid_task, mach_vm_trunc_page(amfid_base + amfid_OFFSET_MISValidate_symbol), 0x4000, 0, VM_PROT_READ|VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) {
        util_error("%s: kr %d: %s", "vm_protect", kr, mach_error_string(kr));
    }
    uintptr_t redirect_pc = 0xffffff8041414141; // for throw invalid-addr-access exception
    uintptr_t old_p, new_p;
    old_p = task_read64(amfid_task, amfid_base + amfid_OFFSET_MISValidate_symbol);
    task_write64(amfid_task, amfid_base + amfid_OFFSET_MISValidate_symbol, redirect_pc);
    new_p = task_read64(amfid_task, amfid_base + amfid_OFFSET_MISValidate_symbol);
    util_info("old %#lx -> new %#lx", old_p, new_p);
}


