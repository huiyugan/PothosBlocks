// Copyright (c) 2014-2016 Josh Blum
//                    2020 Nicholas Corgan
// SPDX-License-Identifier: BSL-1.0

#include "MemoryMappedBufferContainer.hpp"

#include <Pothos/Framework.hpp>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _MSC_VER
#include <io.h>
#else
#include <unistd.h>
#endif //_MSC_VER
#include <stdio.h>
#include <cerrno>

#ifndef O_BINARY
#define O_BINARY 0
#endif

#include <Poco/Logger.h>

/***********************************************************************
 * |PothosDoc Binary File Source
 *
 * Read data from a file and write it to an output stream on port 0.
 *
 * |category /Sources
 * |category /File IO
 * |keywords source binary file
 *
 * |param dtype[Data Type] The output data type.
 * |widget DTypeChooser(float=1,cfloat=1,int=1,cint=1,uint=1,cuint=1,dim=1)
 * |default "complex_float64"
 * |preview disable
 *
 * |param path[File Path] The path to the input file.
 * |default ""
 * |widget FileEntry(mode=open)
 *
 * |param rewind[Auto Rewind] Enable automatic file rewind.
 * When rewind is enabled, the binary file source will stream from the beginning
 * of the file after the end of file is reached.
 * |default false
 * |option [Disabled] false
 * |option [Enabled] true
 * |preview valid
 *
 * |factory /blocks/binary_file_source(dtype)
 * |setter setFilePath(path)
 * |setter setAutoRewind(rewind)
 **********************************************************************/
class BinaryFileSource : public Pothos::Block
{
public:
    static Block *make(const Pothos::DType& dtype)
    {
        return new BinaryFileSource(dtype);
    }

    BinaryFileSource(const Pothos::DType& dtype):
        _mmapSharedBuff(Pothos::SharedBuffer::null()),
        _rewind(false),
        _offset(0),
        _dtype(dtype),
        _workedOnce(false)
    {
        this->setupOutput(0, dtype);
        this->registerCall(this, POTHOS_FCN_TUPLE(BinaryFileSource, setFilePath));
        this->registerCall(this, POTHOS_FCN_TUPLE(BinaryFileSource, setAutoRewind));
    }

    void setFilePath(const std::string &path)
    {
        _path = path;
        this->deactivate();
        this->activate();
    }

    void setAutoRewind(const bool rewind)
    {
        _rewind = rewind;
    }

    void activate(void)
    {
        // Validation performed here
        auto containerSPtr = MemoryMappedBufferContainer::make(
                                 _path,
                                 true /*readable*/,
                                 false /*writable*/);
        _mmapSharedBuff = Pothos::SharedBuffer(
                              reinterpret_cast<size_t>(containerSPtr->buffer()),
                              containerSPtr->length(),
                              containerSPtr);
    }

    void deactivate(void)
    {
        _mmapSharedBuff = Pothos::SharedBuffer::null();
    }

    void work(void)
    {
        if(!_mmapSharedBuff)
        {
            throw Pothos::RuntimeException("work() called with invalid file.");
        }

        // We're posting, but only post when we'd theoretically be able to produce.
        if(0 == this->workInfo().minElements) return;
        if(!_rewind && _workedOnce) return;

        auto bufferChunk = Pothos::BufferChunk(_mmapSharedBuff);
        bufferChunk.dtype = _dtype;

        this->output(0)->postBuffer(std::move(bufferChunk));

        _workedOnce = true;
    }

private:
    std::string _path;

    Pothos::SharedBuffer _mmapSharedBuff;
    bool _rewind;
    size_t _offset;

    Pothos::DType _dtype;
    bool _workedOnce;
};

static Pothos::BlockRegistry registerBinaryFileSource(
    "/blocks/binary_file_source", &BinaryFileSource::make);
