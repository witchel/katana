/**
 * @file tsuba_api.cpp
 *
 * Contains the entry points for interfacing with the tsuba storage server
 */

#include <mutex>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <cassert>

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "galois/Platform.h"

#include "tsuba/tsuba.h"
#include "s3.h"
#include "tsuba_internal.h"

namespace tsuba {

template <typename T>
static T* MmapCast(size_t size, int prot, int flags, int fd, off_t off) {
  void* ret = galois::MmapPopulate(nullptr, size, prot, flags, fd, off);
  if (ret == MAP_FAILED) {
    perror("mmap");
    return nullptr;
  }
  return reinterpret_cast<T*>(ret); /* NOLINT cast needed for mmap interface */
}

static uint8_t* MmapLocalFile(const std::string& filename, uint64_t begin,
                              uint64_t size) {
  int fd = open(filename.c_str(), O_RDONLY, 0);
  if (fd < 0) {
    return nullptr;
  }
  auto* ret = MmapCast<uint8_t>(size, PROT_READ, MAP_SHARED, fd, begin);
  close(fd);
  return ret;
}

static int DoReadS3Part(const std::string& filename, uint8_t* buf,
                        uint64_t begin, uint64_t size) {
  auto [bucket, object] = S3SplitUri(filename);
  return S3DownloadRange(bucket, object, begin, size, buf);
}

static int DoWriteS3(const std::string& filename, const uint8_t* buf,
                     uint64_t size) {
  auto [bucket, object] = S3SplitUri(filename);
  return S3UploadOverwrite(bucket, object, buf, size);
}

static int DoWriteS3Sync(const std::string& filename, const uint8_t* buf,
                         uint64_t size) {
  auto [bucket, object] = S3SplitUri(filename);
  return S3PutSingleSync(bucket, object, buf, size);
}

static int DoWriteS3Async(const std::string& filename, const uint8_t* buf,
                          uint64_t size) {
  auto [bucket, object] = S3SplitUri(filename);
  return S3PutSingleAsync(bucket, object, buf, size);
}

static int DoWriteS3AsyncFinish(const std::string& filename) {
  auto [bucket, object] = S3SplitUri(filename);
  return S3PutSingleAsyncFinish(bucket, object);
}

static int DoWriteS3MultiAsync1(const std::string& filename, const uint8_t* buf,
                                uint64_t size) {
  auto [bucket, object] = S3SplitUri(filename);
  return S3PutMultiAsync1(bucket, object, buf, size);
}
static int DoWriteS3MultiAsync2(const std::string& filename) {
  auto [bucket, object] = S3SplitUri(filename);
  return S3PutMultiAsync2(bucket, object);
}
static int DoWriteS3MultiAsync3(const std::string& filename) {
  auto [bucket, object] = S3SplitUri(filename);
  return S3PutMultiAsync3(bucket, object);
}
static int DoWriteS3MultiAsyncFinish(const std::string& filename) {
  auto [bucket, object] = S3SplitUri(filename);
  return S3PutMultiAsyncFinish(bucket, object);
}

static uint8_t* AllocAndReadS3(const std::string& filename, uint64_t begin,
                               uint64_t size) {
  auto* ret = MmapCast<uint8_t>(size, PROT_READ | PROT_WRITE,
                                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (ret == nullptr) {
    std::cerr << "alloc for s3 read failed" << std::endl;
    return nullptr;
  }
  if (DoReadS3Part(filename, ret, begin, size)) {
    munmap(ret, size);
    ret = nullptr;
    std::cerr << "do_read_s3_part failed" << std::endl;
  }
  return ret;
}

class MappingDesc {
  uint8_t* ptr_{nullptr};
  size_t size_{0};
  bool valid_ = false;

public:
  MappingDesc(const MappingDesc&) = delete;
  MappingDesc& operator=(const MappingDesc&) = delete;
  MappingDesc()                              = default;
  MappingDesc(MappingDesc&& other) noexcept
      : ptr_(other.ptr_), size_(other.size_), valid_(other.valid_) {
    other.valid_ = false;
  }
  MappingDesc& operator=(MappingDesc&& other) noexcept {
    if (&other != this) {
      if (valid_) {
        munmap(ptr_, size_);
      }
      ptr_         = other.ptr_;
      size_        = other.size_;
      valid_       = other.valid_;
      other.valid_ = false;
    }
    return *this;
  }
  int Init(const std::string& uri, uint64_t offset, size_t size) {
    assert(!valid_);
    if (IsUri(uri)) {
      ptr_ = AllocAndReadS3(uri, offset, size);
    } else {
      ptr_ = MmapLocalFile(uri, offset, size);
    }
    if (ptr_ == nullptr) {
      return -1;
    }
    valid_ = true;
    size_  = size;
    return 0;
  }
  ~MappingDesc() {
    if (valid_) {
      munmap(ptr_, size_);
    }
  }
  uint8_t* ptr() { return ptr_; }
};

static std::mutex allocated_memory_mutex;
static std::unordered_map<uint8_t*, MappingDesc> allocated_memory;

int Init() { return S3Init(); }
void Fini() { return S3Fini(); }

int Open(const std::string& uri) {
  auto [bucket_name, object_name] = S3SplitUri(uri);
  if (bucket_name.empty() || object_name.empty()) {
    return ERRNO_RET(EINVAL, -1);
  }
  auto result = S3Open(bucket_name, object_name);
  if (!result) {
    return -1;
  }
  return result.value();
}

// Return 1: This is an S3 URL
// Otherwise process as file and return 0 for success and -1 for fail
static int S3OrDoFile(const std::string& uri, const uint8_t* data,
                      uint64_t size) {
  if (IsUri(uri)) {
    return 1;
  }
  std::ofstream ofile(uri);
  if (!ofile.good()) {
    return -1;
  }
  ofile.write(reinterpret_cast<const char*>(data), size); /* NOLINT */
  if (!ofile.good()) {
    return -1;
  }
  return 0;
}

int Store(const std::string& uri, const uint8_t* data, uint64_t size) {
  int ret = S3OrDoFile(uri, data, size);
  if (ret) {
    ret = DoWriteS3(uri, data, size);
  }
  return ret;
}

int StoreSync(const std::string& uri, const uint8_t* data, uint64_t size) {
  int ret = S3OrDoFile(uri, data, size);
  if (ret) {
    ret = DoWriteS3Sync(uri, data, size);
  }
  return ret;
}

int StoreAsync(const std::string& uri, const uint8_t* data, uint64_t size) {
  int ret = S3OrDoFile(uri, data, size);
  if (ret) {
    ret = DoWriteS3Async(uri, data, size);
  }
  return ret;
}

int StoreAsyncFinish(const std::string& uri) {
  if (!IsUri(uri)) {
    return 0;
  }
  return DoWriteS3AsyncFinish(uri);
}

int StoreMultiAsync1(const std::string& uri, const uint8_t* data,
                     uint64_t size) {
  int ret = S3OrDoFile(uri, data, size);
  if (ret) {
    ret = DoWriteS3MultiAsync1(uri, data, size);
  }
  return ret;
}
int StoreMultiAsync2(const std::string& uri) {
  if (!IsUri(uri)) {
    return 0;
  }
  return DoWriteS3MultiAsync2(uri);
}
int StoreMultiAsync3(const std::string& uri) {
  if (!IsUri(uri)) {
    return 0;
  }
  return DoWriteS3MultiAsync3(uri);
}

int StoreMultiAsyncFinish(const std::string& uri) {
  if (!IsUri(uri)) {
    return 0;
  }
  return DoWriteS3MultiAsyncFinish(uri);
}

int Peek(const std::string& filename, uint8_t* result_buffer, uint64_t begin,
         uint64_t size) {
  if (!IsUri(filename)) {
    std::ifstream infile(filename);
    if (!infile.good()) {
      return -1;
    }
    infile.seekg(begin);
    if (!infile.good()) {
      return -1;
    }
    infile.read(reinterpret_cast<char*>(result_buffer), size); /* NOLINT */
    if (!infile.good()) {
      return -1;
    }
    return 0;
  }
  return DoReadS3Part(filename, result_buffer, begin, size);
}

int Stat(const std::string& filename, StatBuf* s_buf) {
  if (!IsUri(filename)) {
    struct stat local_s_buf;
    if (int ret = stat(filename.c_str(), &local_s_buf); ret) {
      return ret;
    }
    s_buf->size = local_s_buf.st_size;
    return 0;
  }
  auto [bucket_name, object_name] = S3SplitUri(std::string(filename));
  return S3GetSize(bucket_name, object_name, &s_buf->size);
}

uint8_t* Mmap(const std::string& filename, uint64_t begin, uint64_t size) {
  MappingDesc new_mapping;
  if (int err = new_mapping.Init(filename, begin, size); err) {
    return nullptr;
  }
  std::lock_guard<std::mutex> guard(allocated_memory_mutex);
  auto [it, inserted] =
      allocated_memory.emplace(new_mapping.ptr(), std::move(new_mapping));
  if (!inserted) {
    std::cerr << "Failed to emplace! (impossible?) " __FILE__ ":" << __LINE__
              << std::endl;
    return nullptr;
  }
  return it->second.ptr();
}

int Munmap(uint8_t* ptr) {
  std::lock_guard<std::mutex> guard(allocated_memory_mutex);
  if (allocated_memory.erase(ptr) != 1) {
    std::cerr << "WARNING: passed unknown pointer to tsuba_munmap" << std::endl;
    return -22; // EINVAL
  }
  return 0;
}

} /* namespace tsuba */
