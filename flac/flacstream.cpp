#include "./flacstream.h"
#include "./flacmetadata.h"

#include "../vorbis/vorbiscomment.h"

#include "../exceptions.h"
#include "../mediafileinfo.h"
#include "../mediaformat.h"

#include "resources/config.h"

#include <c++utilities/io/copy.h>

#include <sstream>

using namespace std;
using namespace IoUtilities;
using namespace ConversionUtilities;
using namespace ChronoUtilities;

namespace Media {

/*!
 * \class Media::FlacStream
 * \brief Implementation of Media::AbstractTrack for raw FLAC streams.
 */

/*!
 * \brief Constructs a new track for the specified \a mediaFileInfo at the specified \a startOffset.
 *
 * The stream of the \a mediaFileInfo instance is used as input stream.
 */
FlacStream::FlacStream(MediaFileInfo &mediaFileInfo, uint64 startOffset) :
    AbstractTrack(mediaFileInfo.stream(), startOffset),
    m_mediaFileInfo(mediaFileInfo),
    m_paddingSize(0),
    m_streamOffset(0)
{
    m_mediaType = MediaType::Audio;
}

/*!
 * \brief Creates a new Vorbis comment for the stream.
 * \remarks Just returns the current Vorbis comment if already present.
 */
VorbisComment *FlacStream::createVorbisComment()
{
    if(!m_vorbisComment) {
        m_vorbisComment = make_unique<VorbisComment>();
    }
    return m_vorbisComment.get();
}

/*!
 * \brief Removes the assigned Vorbis comment if one is assigned; does nothing otherwise.
 * \returns Returns whether there were a Vorbis comment assigned.
 */
bool FlacStream::removeVorbisComment()
{
    if(m_vorbisComment) {
        m_vorbisComment.reset();
        return true;
    } else {
        return false;
    }
}

void FlacStream::internalParseHeader()
{
    static const string context("parsing raw FLAC header");
    if(!m_istream) {
        throw NoDataFoundException();
    }

    m_istream->seekg(m_startOffset, ios_base::beg);
    char buffer[0x22];

    // check signature
    if(m_reader.readUInt32BE() == 0x664C6143) {
        m_format = GeneralMediaFormat::Flac;

        // parse meta data blocks
        for(FlacMetaDataBlockHeader header; !header.isLast(); ) {
            // parse block header
            m_istream->read(buffer, 4);
            header.parseHeader(buffer);

            // remember start offset
            const auto startOffset = m_istream->tellg();

            // parse relevant meta data
            switch(static_cast<FlacMetaDataBlockType>(header.type())) {
            case FlacMetaDataBlockType::StreamInfo:
                if(header.dataSize() >= 0x22) {
                    m_istream->read(buffer, 0x22);
                    FlacMetaDataBlockStreamInfo streamInfo;
                    streamInfo.parse(buffer);
                    m_channelCount = streamInfo.channelCount();
                    m_samplingFrequency = streamInfo.samplingFrequency();
                    m_sampleCount = streamInfo.totalSampleCount();
                    m_bitsPerSample = streamInfo.bitsPerSample();
                    m_duration = TimeSpan::fromSeconds(static_cast<double>(m_sampleCount) / m_samplingFrequency);
                } else {
                    addNotification(NotificationType::Critical, "\"METADATA_BLOCK_STREAMINFO\" is truncated and will be ignored.", context);
                }
                break;

            case FlacMetaDataBlockType::VorbisComment:
                // parse Vorbis comment
                // if more than one comment exist, simply thread those comments as one
                if(!m_vorbisComment) {
                    m_vorbisComment = make_unique<VorbisComment>();
                }
                try {
                    m_vorbisComment->parse(*m_istream, header.dataSize(), VorbisCommentFlags::NoSignature | VorbisCommentFlags::NoFramingByte);
                } catch(const Failure &) {
                    // error is logged via notifications, just continue with the next metadata block
                }
                break;

            case FlacMetaDataBlockType::Picture:
                try {
                    // parse the cover
                    VorbisCommentField coverField;
                    coverField.setId(m_vorbisComment->fieldId(KnownField::Cover));
                    FlacMetaDataBlockPicture picture(coverField.value());
                    picture.parse(*m_istream, header.dataSize());
                    coverField.setTypeInfo(picture.pictureType());

                    if(coverField.value().isEmpty()) {
                        addNotification(NotificationType::Warning, "\"METADATA_BLOCK_PICTURE\" contains no picture.", context);
                    } else {
                        // add the cover to the Vorbis comment
                        if(!m_vorbisComment) {
                            // create one if none exists yet
                            m_vorbisComment = make_unique<VorbisComment>();
                            m_vorbisComment->setVendor(TagValue(APP_NAME " v" APP_VERSION, TagTextEncoding::Utf8));
                        }
                        m_vorbisComment->fields().insert(make_pair(coverField.id(), move(coverField)));
                    }

                } catch(const TruncatedDataException &) {
                    addNotification(NotificationType::Critical, "\"METADATA_BLOCK_PICTURE\" is truncated and will be ignored.", context);
                }
                break;

            case FlacMetaDataBlockType::Padding:
                m_paddingSize += 4 + header.dataSize();
                break;

            default:
                ;
            }

            // seek to next block
            m_istream->seekg(startOffset + static_cast<decltype(startOffset)>(header.dataSize()));

            // TODO: check first FLAC frame
        }

        m_streamOffset = m_istream->tellg();

    } else {
        addNotification(NotificationType::Critical, "Signature (fLaC) not found.", context);
        throw InvalidDataException();
    }
}

/*!
 * \brief Writes the FLAC metadata header to the specified \a outputStream.
 *
 * This basically copies all "METADATA_BLOCK_HEADER" of the current stream to the specified \a outputStream, except:
 *
 *  - Vorbis comment is updated.
 *  - "METADATA_BLOCK_PICTURE" are updated.
 *  - Padding is skipped
 *
 * \returns Returns the start offset of the last "METADATA_BLOCK_HEADER" withing \a outputStream.
 */
uint32 FlacStream::makeHeader(ostream &outputStream)
{
    istream &originalStream = m_mediaFileInfo.stream();
    originalStream.seekg(m_startOffset + 4);
    CopyHelper<512> copy;

    // write signature
    BE::getBytes(static_cast<uint32>(0x664C6143u), copy.buffer());
    outputStream.write(copy.buffer(), 4);

    uint32 lastStartOffset = 0;

    // write meta data blocks which don't need to be adjusted
    for(FlacMetaDataBlockHeader header; !header.isLast(); ) {
        // parse block header
        m_istream->read(copy.buffer(), 4);
        header.parseHeader(copy.buffer());

        // parse relevant meta data
        switch(static_cast<FlacMetaDataBlockType>(header.type())) {
        case FlacMetaDataBlockType::VorbisComment:
        case FlacMetaDataBlockType::Picture:
        case FlacMetaDataBlockType::Padding:
            m_istream->seekg(header.dataSize(), ios_base::cur);
            break; // written separately/ignored
        default:
            m_istream->seekg(-4, ios_base::cur);
            lastStartOffset = outputStream.tellp();
            copy.copy(originalStream, outputStream, 4 + header.dataSize());
        }
    }

    // write Vorbis comment
    if(m_vorbisComment) {
        // leave 4 bytes space for the "METADATA_BLOCK_HEADER"
        lastStartOffset = outputStream.tellp();
        outputStream.write(copy.buffer(), 4);

        // determine cover ID since covers must be written separately
        const auto coverId = m_vorbisComment->fieldId(KnownField::Cover);

        // write Vorbis comment
        m_vorbisComment->make(outputStream, VorbisCommentFlags::NoSignature | VorbisCommentFlags::NoFramingByte | VorbisCommentFlags::NoCovers);

        // write "METADATA_BLOCK_HEADER"
        const uint32 endOffset = outputStream.tellp();
        FlacMetaDataBlockHeader header;
        header.setType(FlacMetaDataBlockType::VorbisComment);
        header.setDataSize(endOffset - lastStartOffset - 4);
        header.setLast(!m_vorbisComment->hasField(coverId));
        outputStream.seekp(lastStartOffset);
        header.makeHeader(outputStream);
        outputStream.seekp(endOffset);

        // write cover fields separately as "METADATA_BLOCK_PICTURE"
        if(!header.isLast()) {
            header.setType(FlacMetaDataBlockType::Picture);
            const auto coverFields = m_vorbisComment->fields().equal_range(coverId);
            for(auto i = coverFields.first; i != coverFields.second; ) {
                lastStartOffset = outputStream.tellp();
                FlacMetaDataBlockPicture pictureBlock(i->second.value());
                pictureBlock.setPictureType(i->second.typeInfo());
                header.setDataSize(pictureBlock.requiredSize());
                header.setLast(++i == coverFields.second);
                header.makeHeader(outputStream);
                pictureBlock.make(outputStream);
            }
        }
    }

    return lastStartOffset;
}

/*!
 * \brief Writes padding of the specified \a size to the specified \a stream.
 * \remarks Size must be at least 4 bytes.
 */
void FlacStream::makePadding(ostream &stream, uint32 size, bool isLast)
{
    // make header
    FlacMetaDataBlockHeader header;
    header.setType(FlacMetaDataBlockType::Padding);
    header.setLast(isLast);
    header.setDataSize(size -= 4);
    header.makeHeader(stream);

    // write zeroes
    for(; size; --size) {
        stream.put(0);
    }
}

}
