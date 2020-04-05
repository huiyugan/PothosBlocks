// Copyright (c) 2020 Nicholas Corgan
// SPDX-License-Identifier: BSL-1.0

#include "MemoryMappedBufferContainer.hpp"

#include <Pothos/Exception.hpp>

#include <Poco/File.h>
#include <Poco/Logger.h>
#include <Poco/Platform.h>

static Poco::Logger& getLogger()
{
    static auto& logger = Poco::Logger::get("MemoryuMappedBufferContainer");
    return logger;
}

//
// UNIX (TODO: support writing to nonexistent file)
//
#ifdef POCO_OS_FAMILY_UNIX

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

template <typename ExcType = Pothos::RuntimeException>
static void throwErrnoOnFailure(int code, const char* context)
{
    if(code < 0) throw ExcType(context, ::strerror(errno));
}

static void logErrnoOnFailure(int code, const char* context)
{
    if(code < 0)
    {
        poco_error_f2(
            getLogger(),
            "%s: %s",
            std::string(context),
            std::string(::strerror(errno)));
    }
}

class MemoryMappedBufferContainer::Impl
{
public:
    Impl(const std::string& filepath,
         bool readable,
         bool writable): _buffer(nullptr), _length(0)
    {
        int openFlags = 0;
        if(readable && writable) openFlags = O_RDWR;
        else if(readable)        openFlags = O_RDONLY;
        else if(writable)        openFlags = O_WRONLY;

        int mmapProt = 0;
        if(readable) mmapProt |= PROT_READ;
        if(writable) mmapProt |= PROT_WRITE;

        constexpr int mmapFlags = MAP_SHARED;

        _length = Poco::File(filepath).getSize();
        if(0 == _length)
        {
            throw Pothos::InvalidArgumentException("Empty files not supported.");
        }

        int fd = 0;
        throwErrnoOnFailure<Pothos::OpenFileException>(
            (fd = ::open(filepath.c_str(), openFlags)),
            "open");

        _buffer = ::mmap(nullptr, _length, mmapProt, mmapFlags, fd, 0);
        if(!_buffer || (MAP_FAILED == _buffer))
        {
            throw Pothos::IOException("mmap", ::strerror(errno));
        }

        throwErrnoOnFailure<Pothos::FileException>(
            ::close(fd),
            "close");
    }

    ~Impl()
    {
        logErrnoOnFailure(
            ::munmap(_buffer, _length),
            "munmap");
    }

    void* buffer() const
    {
        return _buffer;
    }

    size_t length() const
    {
        return _length;
    }

private:
    void* _buffer;
    size_t _length;
};

#endif

//
// Windows
//
#ifdef POCO_OS_FAMILY_WINDOWS
#error Unimplemented
#endif

//
// OS-independent
//

MemoryMappedBufferContainer::SPtr MemoryMappedBufferContainer::make(
    const std::string& filepath,
    bool readable,
    bool writable)
{
    return std::make_shared<MemoryMappedBufferContainer>(filepath, readable, writable);
}

MemoryMappedBufferContainer::MemoryMappedBufferContainer(
    const std::string& filepath,
    bool readable,
    bool writable): _impl(nullptr)
{
    const Poco::File pocoFile(filepath);

    if(!pocoFile.exists())
    {
        throw Pothos::FileNotFoundException(filepath);
    }
    if(readable && !pocoFile.canRead())
    {
        throw Pothos::FileAccessDeniedException(filepath);
    }
    if(writable && !pocoFile.canWrite())
    {
        throw Pothos::FileReadOnlyException(filepath);
    }

    _impl = new Impl(filepath, readable, writable);
}

MemoryMappedBufferContainer::~MemoryMappedBufferContainer()
{
    delete _impl;
}

void* MemoryMappedBufferContainer::buffer() const
{
    return _impl->buffer();
}

size_t MemoryMappedBufferContainer::length() const
{
    return _impl->length();
}
