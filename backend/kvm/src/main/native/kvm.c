#include <assert.h>
#include <sys/mman.h>
#include <sys/errno.h>

#include <fcntl.h>
#include <sys/ioctl.h>

#include <stdio.h>
#include <unistd.h>

#include "kvm.h"

typedef struct kvm_cpu {
  int fd;
  struct kvm_run *run;
  uint32_t offset;
} *t_kvm_cpu;


typedef struct kvm {
  bool is64Bit;
  khash_t(memory) *memory;
  size_t num_page_table_entries;
  void **page_table;
  t_kvm_cpu cpu;
  jobject callback;
  bool stop_request;
  uint64_t sp;
  uint64_t cpacr;
  uint64_t tpidr;
  uint64_t tpidrro;

  ////
  int gKvmFd;
  int gRunSize;
  int gMaxSlots;
  bool gHasPmuV3;
  bool has32Bit;
  int hasMultiAddressSpace;
} *t_kvm;

static jmethodID handleException = NULL;

static int check_one_reg(uint64_t reg, int ret) {
  if(ret == 0) {
    return 0;
  }
  switch(errno) {
    case ENOENT:
      fprintf(stderr, "no such register: reg=0x%llx\n", reg);
      break;
    case EINVAL:
      fprintf(stderr, "invalid register ID, or no such register: reg=0x%llx\n", reg);
      break;
    case EPERM:
      fprintf(stderr, "(arm64) register access not allowed before vcpu finalization: reg=0x%llx\n", reg);
      break;
    default:
      fprintf(stderr, "unknown error: reg=0x%llx, errno=%d\n", reg, errno);
      break;
  }
  return ret;
}

hv_return_t hv_vcpu_get_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t *value) {
    struct kvm_one_reg reg_req = {
        .id = reg,
        .addr = (uint64_t)value,
    };
    if (check_one_reg(reg, ioctl(vcpu->fd, KVM_GET_ONE_REG, &reg_req)) < 0) {
        return -1;
    }
    return HV_SUCCESS;
}

hv_return_t hv_vcpu_set_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t value) {
    struct kvm_one_reg reg_req = {
        .id = reg,
        .addr = (uint64_t)&value,
    };
    if (check_one_reg(reg, ioctl(vcpu->fd, KVM_SET_ONE_REG, &reg_req)) < 0) {
        return -1;
    }
    return HV_SUCCESS;
}

hv_return_t hv_vcpu_get_sys_reg(hv_vcpu_t vcpu, hv_sys_reg_t reg, uint64_t *value) {
    struct kvm_one_reg reg_req = {
        .id = reg,
        .addr = (uint64_t)value,
    };
    if (check_one_reg(reg, ioctl(vcpu->fd, KVM_GET_ONE_REG, &reg_req)) < 0) {
        return -1;
    }
    return HV_SUCCESS;
}

hv_return_t hv_vcpu_set_sys_reg(hv_vcpu_t vcpu, hv_sys_reg_t reg, uint64_t value) {
    struct kvm_one_reg reg_req = {
        .id = reg,
        .addr = (uint64_t)&value,
    };
    if (check_one_reg(reg, ioctl(vcpu->fd, KVM_SET_ONE_REG, &reg_req)) < 0) {
        return -1;
    }
    return HV_SUCCESS;
}

hv_return_t hv_vcpu_get_simd_fp_reg(hv_vcpu_t vcpu, hv_simd_fp_reg_t reg, hv_simd_fp_uchar16_t *value) {
    struct kvm_one_reg reg_req = {
        .id = reg,
        .addr = (uint64_t)value,
    };
    if (check_one_reg(reg, ioctl(vcpu->fd, KVM_GET_ONE_REG, &reg_req)) < 0) {
        return -1;
    }
    return HV_SUCCESS;
}

hv_return_t hv_vcpu_set_simd_fp_reg(hv_vcpu_t vcpu, hv_simd_fp_reg_t reg, hv_simd_fp_uchar16_t value) {
    struct kvm_one_reg reg_req = {
        .id = reg,
        .addr = (uint64_t)&value,
    };
    if (check_one_reg(reg, ioctl(vcpu->fd, KVM_SET_ONE_REG, &reg_req)) < 0) {
        return -1;
    }
    return HV_SUCCESS;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    getMaxSlots
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_getMaxSlots
  (JNIEnv *env, jclass clazz, jlong handle) {
  t_kvm kvm = (t_kvm) handle;
  return kvm->gMaxSlots;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    getPageSize
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_getPageSize
  (JNIEnv *env, jclass clazz) {
  long sz = sysconf(_SC_PAGESIZE);
  return (jint) sz;
}

static char *get_memory_page(khash_t(memory) *memory, uint64_t vaddr, size_t num_page_table_entries, void **page_table) {
    uint64_t idx = vaddr >> PAGE_BITS;
    if(page_table && idx < num_page_table_entries) {
      return (char *)page_table[idx];
    }
    uint64_t base = vaddr & ~KVM_PAGE_MASK;
    khiter_t k = kh_get(memory, memory, base);
    if(k == kh_end(memory)) {
      return NULL;
    }
    t_memory_page page = kh_value(memory, k);
    return (char *)page->addr;
}

static inline void *get_memory(khash_t(memory) *memory, uint64_t vaddr, size_t num_page_table_entries, void **page_table) {
    char *page = get_memory_page(memory, vaddr, num_page_table_entries, page_table);
    return page ? &page[vaddr & KVM_PAGE_MASK] : NULL;
}

static t_kvm_cpu create_kvm_cpu(t_kvm kvm) {
  struct kvm_vcpu_init vcpu_init;
  if (ioctl(kvm->gKvmFd, KVM_ARM_PREFERRED_TARGET, &vcpu_init) == -1) {
    fprintf(stderr, "KVM_ARM_PREFERRED_TARGET failed.\n");
    abort();
    return NULL;
  }

  int fd = ioctl(kvm->gKvmFd, KVM_CREATE_VCPU, 0);
  if (fd == -1) {
    fprintf(stderr, "KVM_CREATE_VCPU failed.\n");
    abort();
    return NULL;
  }

  // ask for psci 0.2
  vcpu_init.features[0] |= 1UL << KVM_ARM_VCPU_PSCI_0_2;
  if(kvm->gHasPmuV3) {
    vcpu_init.features[0] |= 1UL << KVM_ARM_VCPU_PMU_V3;
  }
  if (ioctl(fd, KVM_ARM_VCPU_INIT, &vcpu_init) == -1) {
    fprintf(stderr, "KVM_ARM_VCPU_INIT failed.\n");
    abort();
    return NULL;
  }
  struct kvm_run *run = mmap(NULL, kvm->gRunSize, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (run == MAP_FAILED) {
    fprintf(stderr, "init kvm_run failed.\n");
    abort();
    return NULL;
  }
  t_kvm_cpu cpu = (t_kvm_cpu) calloc(1, sizeof(struct kvm_cpu));
  cpu->fd = fd;
  cpu->run = run;

  //ask for pmu,see https://github.com/OpenMPDK/SMDK/blob/1f9726b3c43b41885cbfd3955f5d9a7aa3a286cb/lib/linux-6.9-smdk/tools/testing/selftests/kvm/aarch64/vpmu_counter_access.c#L423
  if(kvm->gHasPmuV3) {
    struct kvm_device_attr init_attr = {
        .group = KVM_ARM_VCPU_PMU_V3_CTRL,
        .attr = KVM_ARM_VCPU_PMU_V3_INIT,
    };
    /* Initialize vPMU */
    if (ioctl(fd, KVM_SET_DEVICE_ATTR, &init_attr) == -1){
        fprintf(stderr, "KVM_SET_DEVICE_ATTR init_attr failed.\n");
    }
  }

  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_VBAR_EL1, REG_VBAR_EL1));
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SCTLR_EL1, 0x4c5d864));
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_CNTV_CVAL_EL0, 0x0));
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_CNTV_CTL_EL0, 0x0));
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_CNTKCTL_EL1, 0x0));
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_MIDR_EL1, 0x410fd083));
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SP_EL1, MMIO_TRAP_ADDRESS));
  //HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_ID_AA64MMFR0_EL1, 0x5));
  //HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_ID_AA64MMFR2_EL1, 0x10000));


  if(kvm->is64Bit) {
//      vcpu->HV_SYS_REG_HCR_EL2 |= (1LL << HCR_EL2$DC); // set stage 1 as normal memory
    return cpu;
  } else {
    if(kvm->has32Bit) {
      return cpu;
    } else {
      fprintf(stderr, "KVM_CAP_ARM_EL1_32BIT unavailable\n");
      abort();
      return NULL;
    }
  }
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    setKvmCallback
 * Signature: (JLcom/github/unidbg/arm/backend/kvm/KvmCallback;)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_setKvmCallback
  (JNIEnv *env, jclass clazz, jlong handle, jobject callback) {
  t_kvm kvm = (t_kvm) handle;
  kvm->callback = (*env)->NewGlobalRef(env, callback);
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    nativeInitialize
 * Signature: (Z)J
 */
JNIEXPORT jlong JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_nativeInitialize
  (JNIEnv *env, jclass clazz, jboolean is64Bit) {
  t_kvm kvm = (t_kvm) calloc(1, sizeof(struct kvm));
  if(kvm == NULL) {
    fprintf(stderr, "calloc kvm failed: size=%lu\n", sizeof(struct kvm));
    abort();
    return 0;
  }

  //kvm initialize
  int kvm_open = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if(kvm_open == -1) {
    fprintf(stderr, "open /dev/kvm failed.\n");
    abort();
    return 0;
  }

  int api_ver = ioctl(kvm_open, KVM_GET_API_VERSION, NULL);
  if(api_ver != KVM_API_VERSION) {
    fprintf(stderr, "Got KVM api version %d, expected %d\n", api_ver, KVM_API_VERSION);
    abort();
    return 0;
  }

  int ret = ioctl(kvm_open, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
  if (!ret) {
    fprintf(stderr, "kvm user memory capability unavailable\n");
    abort();
    return 0;
  }

  ret = ioctl(kvm_open, KVM_CHECK_EXTENSION, KVM_CAP_ARM_PSCI_0_2);
  if (!ret) {
    fprintf(stderr, "KVM_CAP_ARM_PSCI_0_2 unavailable\n");
    abort();
    return 0;
  }

  kvm->gHasPmuV3 = ioctl(kvm_open, KVM_CHECK_EXTENSION, KVM_CAP_ARM_PMU_V3) > 0;
  kvm->gRunSize = ioctl(kvm_open, KVM_GET_VCPU_MMAP_SIZE, NULL);
  kvm->gMaxSlots = ioctl(kvm_open, KVM_CHECK_EXTENSION, KVM_CAP_NR_MEMSLOTS);
  kvm->hasMultiAddressSpace = ioctl(kvm_open, KVM_CHECK_EXTENSION, KVM_CAP_MULTI_ADDRESS_SPACE);
  kvm->has32Bit = ioctl(kvm_open, KVM_CHECK_EXTENSION, KVM_CAP_ARM_EL1_32BIT) > 0;

  int fd = ioctl(kvm_open, KVM_CREATE_VM, KVM_VM_TYPE_ARM_IPA_SIZE(0));
  if (fd == -1) {
    fprintf(stderr, "createVM failed\n");
    abort();
    return 0;
  }
  struct kvm_enable_cap cap;
  memset(&cap, 0, sizeof(struct kvm_enable_cap));
  cap.cap = KVM_CAP_ARM_NISV_TO_USER;
  ret = ioctl(fd, KVM_ENABLE_CAP, &cap);
  if (ret == -1) {
    fprintf(stderr, "KVM_CAP_ARM_NISV_TO_USER unavailable,errno = %d\n",errno);
  }
  kvm->gKvmFd = fd;
  close(kvm_open);
  //  printf("initVM fd=%d, gRunSize=0x%x, gMaxSlots=0x%x, hasMultiAddressSpace=%d, has32Bit=%d, gHasPmuV3=%d\n", fd, gRunSize, gMaxSlots, hasMultiAddressSpace, has32Bit, gHasPmuV3);
  //  printf("initVM HV_REG_X0=0x%llx, HV_REG_X1=0x%llx, HV_REG_PC=0x%llx, gKvmFd=%d\n", HV_REG_X0, HV_REG_X1, HV_REG_PC, gKvmFd);
  //kvm initialize end//


  kvm->is64Bit = is64Bit == JNI_TRUE;
  kvm->memory = kh_init(memory);
  if(kvm->memory == NULL) {
    fprintf(stderr, "kh_init memory failed\n");
    abort();
    return 0;
  }
  ret = kh_resize(memory, kvm->memory, 0x1000);
  if(ret == -1) {
    fprintf(stderr, "kh_resize memory failed\n");
    abort();
    return 0;
  }
  kvm->num_page_table_entries = 1ULL << (PAGE_TABLE_ADDRESS_SPACE_BITS - PAGE_BITS);
  size_t size = kvm->num_page_table_entries * sizeof(void*);
  kvm->page_table = (void **)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if(kvm->page_table == MAP_FAILED) {
    fprintf(stderr, "createVM mmap failed[%s->%s:%d] size=0x%zx, errno=%d, msg=%s\n", __FILE__, __func__, __LINE__, size, errno, strerror(errno));
    abort();
    return 0;
  }
  kvm->cpu = create_kvm_cpu(kvm);
  return (jlong) kvm;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    nativeDestroy
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_nativeDestroy
  (JNIEnv *env, jclass clazz, jlong handle) {
  t_kvm kvm = (t_kvm) handle;
  munmap(kvm->cpu->run, kvm->gRunSize);
  close(kvm->cpu->fd);
  free(kvm->cpu);

  khash_t(memory) *memory = kvm->memory;
  khiter_t k = kh_begin(memory);
  for (; k < kh_end(memory); k++) {
    if(kh_exist(memory, k)) {
      t_memory_page page = kh_value(memory, k);
      int ret = munmap(page->addr, KVM_PAGE_SIZE);
      if(ret != 0) {
        fprintf(stderr, "munmap failed[%s->%s:%d]: addr=%p, ret=%d\n", __FILE__, __func__, __LINE__, page->addr, ret);
      }
      free(page);
    }
  }
  kh_destroy(memory, memory);
  if(kvm->callback) {
    (*env)->DeleteGlobalRef(env, kvm->callback);
  }
  if(kvm->page_table) {
    int ret = munmap(kvm->page_table, kvm->num_page_table_entries * sizeof(void*));
    if(ret != 0) {
      fprintf(stderr, "munmap failed[%s->%s:%d]: page_table=%p, ret=%d\n", __FILE__, __func__, __LINE__, kvm->page_table, ret);
    }
  }
  free(kvm);
  close(kvm->gKvmFd);
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    remove_user_memory_region
 * Signature: (JIJJJ)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_remove_1user_1memory_1region
  (JNIEnv *env, jclass clazz, jlong handle, jint slot, jlong guest_phys_addr, jlong memory_size, jlong userspace_addr, jlong vaddr_off) {

  t_kvm kvm = (t_kvm) handle;
  khash_t(memory) *memory = kvm->memory;

  if(memory_size > 0) {
    char *start_addr = (char *) (userspace_addr + vaddr_off);
    int ret = munmap(start_addr, memory_size);
    if(ret != 0) {
      fprintf(stderr, "munmap failed: userspace_addr=0x%llx, memory_size=0x%llx\n", userspace_addr, memory_size);
      return 1;
    }
  }

//  printf("remove_user_memory_region slot=%d, guest_phys_addr=0x%lx, memory_size=0x%lx, userspace_addr=0x%lx, vaddr_off=0x%lx, addr=%p\n", slot, guest_phys_addr, memory_size, userspace_addr, vaddr_off, start_addr);

  struct kvm_userspace_memory_region region = {
    .slot = slot,
    .flags = 0,
    .guest_phys_addr = guest_phys_addr,
    .memory_size = 0,
    .userspace_addr = userspace_addr,
  };
  if (ioctl(kvm->gKvmFd, KVM_SET_USER_MEMORY_REGION, &region) == -1) {
    fprintf(stderr, "set_user_memory_region failed userspace_addr=0x%llx, guest_phys_addr=0x%lx\n", userspace_addr, guest_phys_addr);
    return 2;
  }

  uint64_t vaddr = guest_phys_addr + vaddr_off;
  for(; vaddr < guest_phys_addr + vaddr_off + memory_size; vaddr += KVM_PAGE_SIZE) {
    uint64_t idx = vaddr >> PAGE_BITS;
    khiter_t k = kh_get(memory, memory, vaddr);
    if(k == kh_end(memory)) {
      fprintf(stderr, "mem_unmap failed[%s->%s:%d]: vaddr=%p\n", __FILE__, __func__, __LINE__, (void*)vaddr);
      return 3;
    }
    if(kvm->page_table && idx < kvm->num_page_table_entries) {
      kvm->page_table[idx] = NULL;
    }
    t_memory_page page = kh_value(memory, k);
    free(page);
    kh_del(memory, memory, k);
  }

  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    set_user_memory_region
 * Signature: (JIJJ)J
 */
JNIEXPORT jlong JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_set_1user_1memory_1region
  (JNIEnv *env, jclass clazz, jlong handle, jint slot, jlong guest_phys_addr, jlong memory_size, jlong userspace_addr) {
  t_kvm kvm = (t_kvm) handle;
  khash_t(memory) *memory = kvm->memory;

  char *start_addr = (char *) userspace_addr;
  if(start_addr == NULL) {
    start_addr = (char *) mmap(NULL, memory_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(start_addr == MAP_FAILED) {
      fprintf(stderr, "mmap failed[%s->%s:%d]: start_addr=%p\n", __FILE__, __func__, __LINE__, start_addr);
      abort();
      return 0L;
    }
  }

//  printf("set_user_memory_region slot=%d, guest_phys_addr=0x%lx, memory_size=0x%lx, userspace_addr=0x%lx, addr=%p\n", slot, guest_phys_addr, memory_size, userspace_addr, start_addr);

  if(guest_phys_addr <= MMIO_TRAP_ADDRESS && guest_phys_addr + memory_size > MMIO_TRAP_ADDRESS) {
    fprintf(stderr, "set_user_memory_region slot=%d, guest_phys_addr=0x%lx, memory_size=0x%lx, userspace_addr=0x%lx, addr=%p\n", slot, guest_phys_addr, memory_size, userspace_addr, start_addr);
    abort();
  }

  struct kvm_userspace_memory_region region = {
    .slot = slot,
    .flags = 0,
    .guest_phys_addr = guest_phys_addr,
    .memory_size = memory_size,
    .userspace_addr = (uint64_t)start_addr,
  };
  if (ioctl(kvm->gKvmFd, KVM_SET_USER_MEMORY_REGION, &region) == -1) {
    fprintf(stderr, "set_user_memory_region failed start_addr=%p, guest_phys_addr=0x%lx\n", start_addr, guest_phys_addr);
    abort();
    return 0L;
  }
//  printf("set_user_memory_region slot=0x%x, guest_phys_addr=0x%llx, memory_size=0x%llx, userspace_addr=%p\n", slot, guest_phys_addr, memory_size, start_addr);

  if(userspace_addr > 0) {
    return userspace_addr;
  }

  int ret;
  uint64_t vaddr = guest_phys_addr;
  for(; vaddr < guest_phys_addr + memory_size; vaddr += KVM_PAGE_SIZE) {
    uint64_t idx = vaddr >> PAGE_BITS;
    if(kh_get(memory, memory, vaddr) != kh_end(memory)) {
      fprintf(stderr, "set_user_memory_region failed[%s->%s:%d]: vaddr=%p\n", __FILE__, __func__, __LINE__, (void*)vaddr);
      return 0L;
    }

    void *addr = &start_addr[vaddr - guest_phys_addr];
//    printf("set_user_memory_region vaddr=0x%llx addr=%p\n", vaddr, addr);
    if(kvm->page_table && idx < kvm->num_page_table_entries) {
      kvm->page_table[idx] = addr;
    } else {
      fprintf(stderr, "guest_phys_addr warning[%s->%s:%d]: addr=%p, page_table=%p, idx=%llu, num_page_table_entries=%zu\n", __FILE__, __func__, __LINE__, (void*)addr, kvm->page_table, idx, kvm->num_page_table_entries);
    }
    khiter_t k = kh_put(memory, memory, vaddr, &ret);
    t_memory_page page = (t_memory_page) calloc(1, sizeof(struct memory_page));
    if(page == NULL) {
      fprintf(stderr, "calloc page failed: size=%lu\n", sizeof(struct memory_page));
      abort();
      return 0L;
    }
    page->addr = addr;
    kh_value(memory, k) = page;
  }

  return (jlong) start_addr;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_read_cpacr_el1
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1read_1cpacr_1el1
  (JNIEnv *env, jclass clazz, jlong handle) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  uint64_t cpacr = 0;
  HYP_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_CPACR_EL1, &cpacr));
  return cpacr;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_set_cpacr_el1
 * Signature: (JJ)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1set_1cpacr_1el1
  (JNIEnv *env, jclass clazz, jlong handle, jlong value) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_CPACR_EL1, value));
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_set_fpexc
 * Signature: (JJ)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1set_1fpexc
  (JNIEnv *env, jclass clazz, jlong handle, jlong value) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_FPEXC32_EL2, value));
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_set_sp64
 * Signature: (JJ)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1set_1sp64
  (JNIEnv *env, jclass clazz, jlong handle, jlong value) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SP_EL0, value));
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_read_sp64
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1read_1sp64
  (JNIEnv *env, jclass clazz, jlong handle) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  uint64_t sp = 0;
  HYP_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_SP_EL0, &sp));
  return sp;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_read_pc64
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1read_1pc64
  (JNIEnv *env, jclass clazz, jlong handle) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  uint64_t pc = 0;
  HYP_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_ELR_EL1, &pc));
  return pc;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_read_nzcv
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1read_1nzcv
  (JNIEnv *env, jclass clazz, jlong handle) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  uint64_t cpsr = 0;
  HYP_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_SPSR_EL1, &cpsr));
  return cpsr;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_set_tpidr_el0
 * Signature: (JJ)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1set_1tpidr_1el0
  (JNIEnv *env, jclass clazz, jlong handle, jlong value) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_TPIDR_EL0, value));
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_set_nzcv
 * Signature: (JJ)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1set_1nzcv
  (JNIEnv *env, jclass clazz, jlong handle, jlong value) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SPSR_EL1, value));
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_set_elr_el1
 * Signature: (JJ)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1set_1elr_1el1
  (JNIEnv *env, jclass clazz, jlong handle, jlong value) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_ELR_EL1, value));
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_set_tpidrro_el0
 * Signature: (JJ)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1set_1tpidrro_1el0
  (JNIEnv *env, jclass clazz, jlong handle, jlong value) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_TPIDRRO_EL0, value));
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    mem_write
 * Signature: (JJ[B)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_mem_1write
  (JNIEnv *env, jclass clazz, jlong handle, jlong address, jbyteArray bytes) {
  jsize size = (*env)->GetArrayLength(env, bytes);
  jbyte *data = (*env)->GetByteArrayElements(env, bytes, NULL);
  t_kvm kvm = (t_kvm) handle;
  khash_t(memory) *memory = kvm->memory;
  char *src = (char *)data;
  uint64_t vaddr_end = address + size;
  uint64_t vaddr = address & ~KVM_PAGE_MASK;
  for(; vaddr < vaddr_end; vaddr += KVM_PAGE_SIZE) {
    uint64_t start = vaddr < address ? address - vaddr : 0;
    uint64_t end = vaddr + KVM_PAGE_SIZE <= vaddr_end ? KVM_PAGE_SIZE : (vaddr_end - vaddr);
    uint64_t len = end - start;
    char *addr = get_memory_page(memory, vaddr, kvm->num_page_table_entries, kvm->page_table);
    if(addr == NULL) {
      fprintf(stderr, "mem_write failed[%s->%s:%d]: vaddr=%p\n", __FILE__, __func__, __LINE__, (void*)vaddr);
      return 1;
    }
    char *dest = &addr[start];
//    printf("mem_write address=%p, vaddr=%p, start=%ld, len=%ld, addr=%p, dest=%p\n", (void*)address, (void*)vaddr, start, len, addr, dest);
    memcpy(dest, src, len);
    src += len;
  }
  (*env)->ReleaseByteArrayElements(env, bytes, data, JNI_ABORT);
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    mem_read
 * Signature: (JJI)[B
 */
JNIEXPORT jbyteArray JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_mem_1read
  (JNIEnv *env, jclass clazz, jlong handle, jlong address, jint size) {
  t_kvm kvm = (t_kvm) handle;
  khash_t(memory) *memory = kvm->memory;
  jbyteArray bytes = (*env)->NewByteArray(env, size);
  uint64_t dest = 0;
  uint64_t vaddr_end = address + size;
  uint64_t vaddr = address & ~KVM_PAGE_MASK;
  for(; vaddr < vaddr_end; vaddr += KVM_PAGE_SIZE) {
    uint64_t start = vaddr < address ? address - vaddr : 0;
    uint64_t end = vaddr + KVM_PAGE_SIZE <= vaddr_end ? KVM_PAGE_SIZE : (vaddr_end - vaddr);
    uint64_t len = end - start;
    char *addr = get_memory_page(memory, vaddr, kvm->num_page_table_entries, kvm->page_table);
    if(addr == NULL) {
      fprintf(stderr, "mem_read failed[%s->%s:%d]: vaddr=%p\n", __FILE__, __func__, __LINE__, (void*)vaddr);
      return NULL;
    }
    jbyte *src = (jbyte *)&addr[start];
    (*env)->SetByteArrayRegion(env, bytes, dest, len, src);
    dest += len;
  }
  return bytes;
}

static hv_reg_t gprs[] = {
  HV_REG_X0,
  HV_REG_X1,
  HV_REG_X2,
  HV_REG_X3,
  HV_REG_X4,
  HV_REG_X5,
  HV_REG_X6,
  HV_REG_X7,
  HV_REG_X8,
  HV_REG_X9,
  HV_REG_X10,
  HV_REG_X11,
  HV_REG_X12,
  HV_REG_X13,
  HV_REG_X14,
  HV_REG_X15,
  HV_REG_X16,
  HV_REG_X17,
  HV_REG_X18,
  HV_REG_X19,
  HV_REG_X20,
  HV_REG_X21,
  HV_REG_X22,
  HV_REG_X23,
  HV_REG_X24,
  HV_REG_X25,
  HV_REG_X26,
  HV_REG_X27,
  HV_REG_X28,
  HV_REG_X29,
  HV_REG_X30,
};
static hv_simd_fp_reg_t fgprs[] = {
  HV_SIMD_FP_REG_Q0,
  HV_SIMD_FP_REG_Q1,
  HV_SIMD_FP_REG_Q2,
  HV_SIMD_FP_REG_Q3,
  HV_SIMD_FP_REG_Q4,
  HV_SIMD_FP_REG_Q5,
  HV_SIMD_FP_REG_Q6,
  HV_SIMD_FP_REG_Q7,
  HV_SIMD_FP_REG_Q8,
  HV_SIMD_FP_REG_Q9,
  HV_SIMD_FP_REG_Q10,
  HV_SIMD_FP_REG_Q11,
  HV_SIMD_FP_REG_Q12,
  HV_SIMD_FP_REG_Q13,
  HV_SIMD_FP_REG_Q14,
  HV_SIMD_FP_REG_Q15,
  HV_SIMD_FP_REG_Q16,
  HV_SIMD_FP_REG_Q17,
  HV_SIMD_FP_REG_Q18,
  HV_SIMD_FP_REG_Q19,
  HV_SIMD_FP_REG_Q20,
  HV_SIMD_FP_REG_Q21,
  HV_SIMD_FP_REG_Q22,
  HV_SIMD_FP_REG_Q23,
  HV_SIMD_FP_REG_Q24,
  HV_SIMD_FP_REG_Q25,
  HV_SIMD_FP_REG_Q26,
  HV_SIMD_FP_REG_Q27,
  HV_SIMD_FP_REG_Q28,
  HV_SIMD_FP_REG_Q29,
  HV_SIMD_FP_REG_Q30,
  HV_SIMD_FP_REG_Q31
};

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_write
 * Signature: (JIJ)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1write
  (JNIEnv *env, jclass clazz, jlong handle, jint index, jlong value) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  hv_reg_t reg = gprs[index];
  HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(cpu, reg, value));
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    reg_read
 * Signature: (JI)J
 */
JNIEXPORT jlong JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_reg_1read
  (JNIEnv *env, jclass clazz, jlong handle, jint index) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;
  uint64_t value = 0;
  hv_reg_t reg = gprs[index];
  HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(cpu, reg, &value));
  return value;
}

static int cpu_loop(JNIEnv *env, t_kvm kvm, t_kvm_cpu cpu) {
  kvm->stop_request = false;
  cpu->offset = 0;

  uint64_t pc = 0;
  uint64_t lr = 0;
  uint64_t sp = 0;
  uint64_t cpsr = 0;
  while(true) {
    if (ioctl(cpu->fd, KVM_RUN, NULL) == -1) {
      hv_vcpu_get_reg(cpu, HV_REG_CPSR, &cpsr);
      hv_vcpu_get_reg(cpu, HV_REG_PC, &pc);
      int code = errno;
      fprintf(stderr, "KVM_RUN failed: reason=%d, cpsr=0x%llx, pc=0x%llx, err = %s(%d)\n", cpu->run->exit_reason, cpsr, pc, strerror(code), code);
      return -1;
    }

    HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(cpu, HV_REG_PC, &pc));
    switch(cpu->run->exit_reason) {
      case KVM_EXIT_ARM_NISV: {
          hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_SP_EL0, &sp);
          fprintf(stderr, "KVM_RUN failed: KVM_EXIT_ARM_NISV error. esr_iss=%p, fault_ipa=%p, sp=%p\n",cpu->run->arm_nisv.esr_iss, cpu->run->arm_nisv.fault_ipa, sp);
          hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SP_EL0, 0x13000000L);
          return -3;
      }
      case KVM_EXIT_MMIO: {
        if(cpu->run->mmio.phys_addr == MMIO_TRAP_ADDRESS || cpu->run->mmio.is_write || cpu->run->mmio.len == 1) {
          uint64_t esr = 0;
          HYP_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_ESR_EL1, &esr));
          uint64_t far = 0;
          HYP_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_FAR_EL1, &far));
          uint64_t elr = 0;
          HYP_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_ELR_EL1, &elr));
          uint64_t cpsr = 0;
          HYP_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_SPSR_EL1, &cpsr));
          jboolean handled = (*env)->CallBooleanMethod(env, kvm->callback, handleException, esr, far, elr, cpsr, pc);
          if ((*env)->ExceptionCheck(env)) {
            return -1;
          }
          if(handled != JNI_TRUE) {
            return 1;
          }
          break;
        }
      }
      default: {
        uint64_t elr = 0;
        HYP_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_ELR_EL1, &elr));
        uint64_t far = 0;
        HYP_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_FAR_EL1, &far));
        fprintf(stderr, "Unexpected VM exit reason: %d, pc=0x%llx, elr=0x%llx, far=0x%llx\n", cpu->run->exit_reason, pc, elr, far);
        return 2;
      }
    }

    if(kvm->stop_request) {
      cpu->offset = 4;
      break;
    }
  }
  return 0;
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    emu_start
 * Signature: (JJ)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_emu_1start
  (JNIEnv *env, jclass clazz, jlong handle, jlong pc) {
  t_kvm kvm = (t_kvm) handle;
  t_kvm_cpu cpu = kvm->cpu;

  if(kvm->is64Bit) {
    uint32_t cpsr = PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT | PSR_MODE_EL0t;
//    printf("emu_start cpsr=0x%x, pc=0x%lx\n", cpsr, pc);
    HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(cpu, HV_REG_CPSR, cpsr));
    HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(cpu, HV_REG_PC, pc - cpu->offset));
  } else {
    bool thumb = pc & 1;
    uint32_t cpsr = PSR_AA32_A_BIT | PSR_AA32_I_BIT | PSR_AA32_F_BIT | PSR_AA32_MODE_USR;
    if(thumb) {
      cpsr |= PSR_AA32_T_BIT;
    }
    HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(cpu, HV_REG_CPSR, cpsr));
    HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(cpu, HV_REG_PC, (uint32_t) (pc & ~1) - cpu->offset));
  }
  return cpu_loop(env, kvm, cpu);
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    emu_stop
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_emu_1stop
  (JNIEnv *env, jclass clazz, jlong handle) {
  t_kvm kvm = (t_kvm) handle;
  kvm->stop_request = true;
  return 0;
}


/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_free(JNIEnv *env, jclass clazz, jlong context) {
  void *ctx = (void *) context;
  free(ctx);
 }

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    context_alloc
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_context_1alloc(JNIEnv *env, jclass clazz, jlong handle){
  t_kvm kvm = (t_kvm) handle;
  if(kvm->is64Bit) {
    void *ctx = malloc(sizeof(struct context64));
    return (jlong) ctx;
  } else {
    fprintf(stderr, "Doesn't support 32 bit\n");
    abort();
    return 0;
  }
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    context_save
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_context_1save(JNIEnv *env, jclass clazz, jlong handle, jlong context){
    t_kvm kvm = (t_kvm) handle;
    t_context64 ctx = (t_context64) context;
    if(kvm->is64Bit) {
        HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(kvm->cpu, HV_SYS_REG_SP_EL0, &ctx->sp));
        HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(kvm->cpu, HV_REG_PC, &ctx->pc));
        HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(kvm->cpu, HV_SYS_REG_TPIDR_EL0, &ctx->tpidr_el0));
        HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(kvm->cpu, HV_SYS_REG_TPIDRRO_EL0, &ctx->tpidrro_el0));
        HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(kvm->cpu, HV_SYS_REG_CPACR_EL1, &ctx->cpacr_el1));
        HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(kvm->cpu, HV_REG_FPCR, &ctx->fpcr));
        HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(kvm->cpu, HV_REG_FPSR, &ctx->fpsr));
        HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(kvm->cpu, HV_REG_CPSR, &ctx->cpsr));
        for(int i = 0; i < 31; i++){
            HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(kvm->cpu, gprs[i], &ctx->registers[i]));
        }
        for(int i = 0; i < 32; i++){
            HYP_ASSERT_SUCCESS(hv_vcpu_get_simd_fp_reg(kvm->cpu, fgprs[i], &ctx->fp_registers[i]));
        }

    }else{
        fprintf(stderr, "Doesn't support 32 bit\n");
        abort();
        return;
    }
}

/*
 * Class:     com_github_unidbg_arm_backend_kvm_Kvm
 * Method:    context_restore
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL Java_com_github_unidbg_arm_backend_kvm_Kvm_context_1restore(JNIEnv *env, jclass clazz, jlong handle, jlong context){
    t_kvm kvm = (t_kvm) handle;
    t_context64 ctx = (t_context64) context;
    if(kvm->is64Bit) {
        HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(kvm->cpu, HV_SYS_REG_SP_EL0, ctx->sp));
        HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(kvm->cpu, HV_REG_PC, ctx->pc));
        HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(kvm->cpu, HV_SYS_REG_TPIDR_EL0, ctx->tpidr_el0));
        HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(kvm->cpu, HV_SYS_REG_TPIDRRO_EL0, ctx->tpidrro_el0));
        HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(kvm->cpu, HV_SYS_REG_CPACR_EL1, ctx->cpacr_el1));
        HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(kvm->cpu, HV_REG_FPCR, ctx->fpcr));
        HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(kvm->cpu, HV_REG_FPSR, ctx->fpsr));
        HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(kvm->cpu, HV_REG_CPSR, ctx->cpsr));
        for(int i = 0; i < 31; i++){
            HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(kvm->cpu, gprs[i], ctx->registers[i]));
        }
        for(int i = 0; i < 32; i++){
            HYP_ASSERT_SUCCESS(hv_vcpu_set_simd_fp_reg(kvm->cpu, fgprs[i], ctx->fp_registers[i]));
        }
    }else{
        fprintf(stderr, "Doesn't support 32 bit\n");
        abort();
        return;
    }
}




JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  JNIEnv *env;
  if (JNI_OK != (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6)) {
    return JNI_ERR;
  }
  jclass cKvmCallback = (*env)->FindClass(env, "com/github/unidbg/arm/backend/kvm/KvmCallback");
  if ((*env)->ExceptionCheck(env)) {
    return JNI_ERR;
  }
  handleException = (*env)->GetMethodID(env, cKvmCallback, "handleException", "(JJJJJ)Z");

  return JNI_VERSION_1_6;
}
