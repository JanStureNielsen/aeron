/*
 * Copyright 2014-2018 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package io.aeron.cluster;

import io.aeron.cluster.codecs.mark.ClusterComponentType;
import io.aeron.cluster.codecs.mark.MarkFileHeaderDecoder;
import io.aeron.cluster.codecs.mark.MarkFileHeaderEncoder;
import io.aeron.cluster.codecs.mark.VarAsciiEncodingEncoder;
import org.agrona.*;
import org.agrona.concurrent.AtomicBuffer;
import org.agrona.concurrent.EpochClock;
import org.agrona.concurrent.UnsafeBuffer;
import org.agrona.concurrent.errors.ErrorConsumer;
import org.agrona.concurrent.errors.ErrorLogReader;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Objects;
import java.util.function.Consumer;

public class ClusterMarkFile implements AutoCloseable
{
    public static final String FILENAME = "cluster-mark.dat";
    public static final int HEADER_LENGTH = 8 * 1024;

    private final MarkFileHeaderDecoder headerDecoder = new MarkFileHeaderDecoder();
    private final MarkFileHeaderEncoder headerEncoder = new MarkFileHeaderEncoder();
    private final MarkFile markFile;
    private final UnsafeBuffer buffer;
    private final UnsafeBuffer errorBuffer;

    public ClusterMarkFile(
        final File file,
        final ClusterComponentType type,
        final int errorBufferLength,
        final EpochClock epochClock,
        final long timeoutMs)
    {
        final boolean markFileExists = file.exists();

        this.markFile = new MarkFile(
            file,
            markFileExists,
            MarkFileHeaderDecoder.versionEncodingOffset(),
            MarkFileHeaderDecoder.activityTimestampEncodingOffset(),
            HEADER_LENGTH + errorBufferLength,
            timeoutMs,
            epochClock,
            (version) ->
            {
                if (version != MarkFileHeaderDecoder.SCHEMA_VERSION)
                {
                    throw new IllegalArgumentException("Mark file version " + version +
                        " does not match software:" + MarkFileHeaderDecoder.SCHEMA_VERSION);
                }
            },
            null);

        this.buffer = markFile.buffer();
        this.errorBuffer = new UnsafeBuffer(this.buffer, HEADER_LENGTH, errorBufferLength);

        headerEncoder.wrap(buffer, 0);
        headerDecoder.wrap(buffer, 0, MarkFileHeaderDecoder.BLOCK_LENGTH, MarkFileHeaderDecoder.SCHEMA_VERSION);

        if (markFileExists)
        {
            final UnsafeBuffer existingErrorBuffer = new UnsafeBuffer(
                this.buffer, headerDecoder.headerLength(), headerDecoder.errorBufferLength());

            saveExistingErrors(file, existingErrorBuffer, System.err);

            errorBuffer.setMemory(0, errorBufferLength, (byte)0);
        }

        final ClusterComponentType existingType = headerDecoder.componentType();

        if (existingType != ClusterComponentType.NULL && existingType != type)
        {
            throw new IllegalStateException(
                "existing Mark file type " + existingType + " not same as required type " + type);
        }

        headerEncoder.componentType(type);
        headerEncoder.headerLength(HEADER_LENGTH);
        headerEncoder.errorBufferLength(errorBufferLength);
        headerEncoder.pid(SystemUtil.getPid());
    }

    public ClusterMarkFile(
        final File directory,
        final String filename,
        final EpochClock epochClock,
        final long timeoutMs,
        final Consumer<String> logger)
    {
        markFile = new MarkFile(
            directory,
            filename,
            MarkFileHeaderDecoder.versionEncodingOffset(),
            MarkFileHeaderDecoder.activityTimestampEncodingOffset(),
            timeoutMs,
            epochClock,
            (version) ->
            {
                if (version != MarkFileHeaderDecoder.SCHEMA_VERSION)
                {
                    throw new IllegalArgumentException("Mark file version " + version +
                        " does not match software:" + MarkFileHeaderDecoder.SCHEMA_VERSION);
                }
            },
            logger);

        this.buffer = markFile.buffer();
        headerDecoder.wrap(buffer, 0, MarkFileHeaderDecoder.BLOCK_LENGTH, MarkFileHeaderDecoder.SCHEMA_VERSION);
        this.errorBuffer = new UnsafeBuffer(
            this.buffer, headerDecoder.headerLength(), headerDecoder.errorBufferLength());
    }

    public void close()
    {
        CloseHelper.close(markFile);
    }

    public void signalReady()
    {
        markFile.signalReady(MarkFileHeaderDecoder.SCHEMA_VERSION);
    }

    public void updateActivityTimestamp(final long nowMs)
    {
        markFile.timestampOrdered(nowMs);
    }

    public long activityTimestampVolatile()
    {
        return markFile.timestampVolatile();
    }

    public MarkFileHeaderEncoder encoder()
    {
        return headerEncoder;
    }

    public MarkFileHeaderDecoder decoder()
    {
        return headerDecoder;
    }

    public UnsafeBuffer buffer()
    {
        return buffer;
    }

    public UnsafeBuffer errorBuffer()
    {
        return errorBuffer;
    }

    public static void saveExistingErrors(final File markFile, final AtomicBuffer errorBuffer, final PrintStream logger)
    {
        try
        {
            final ByteArrayOutputStream baos = new ByteArrayOutputStream();
            final int observations = saveErrorLog(new PrintStream(baos, false, "UTF-8"), errorBuffer);
            if (observations > 0)
            {
                final SimpleDateFormat dateFormat = new SimpleDateFormat("yyyy-MM-dd-HH-mm-ss-SSSZ");
                final String errorLogFilename =
                    markFile.getParent() + '-' + dateFormat.format(new Date()) + "-error.log";

                if (null != logger)
                {
                    logger.println("WARNING: Existing errors saved to: " + errorLogFilename);
                }

                try (FileOutputStream out = new FileOutputStream(errorLogFilename))
                {
                    baos.writeTo(out);
                }
            }
        }
        catch (final Exception ex)
        {
            LangUtil.rethrowUnchecked(ex);
        }
    }

    public static int saveErrorLog(final PrintStream out, final AtomicBuffer errorBuffer)
    {
        final SimpleDateFormat dateFormat = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSSZ");
        final ErrorConsumer errorConsumer = (count, firstTimestamp, lastTimestamp, ex) ->
            out.format(
            "***%n%d observations from %s to %s for:%n %s%n",
            count,
            dateFormat.format(new Date(firstTimestamp)),
            dateFormat.format(new Date(lastTimestamp)),
            ex);

        final int distinctErrorCount = ErrorLogReader.read(errorBuffer, errorConsumer);

        out.format("%n%d distinct errors observed.%n", distinctErrorCount);

        return distinctErrorCount;
    }

    public static void checkHeaderLength(
        final String aeronDirectory,
        final String archiveChannel,
        final String serviceControlChannel,
        final String ingressChannel,
        final String serviceName,
        final String authenticator)
    {
        Objects.requireNonNull(aeronDirectory);
        Objects.requireNonNull(archiveChannel);
        Objects.requireNonNull(serviceControlChannel);

        final int lengthRequired =
            MarkFileHeaderEncoder.BLOCK_LENGTH +
            (6 * VarAsciiEncodingEncoder.lengthEncodingLength()) +
            aeronDirectory.length() +
            archiveChannel.length() +
            serviceControlChannel.length() +
            (null == ingressChannel ? 0 : ingressChannel.length()) +
            (null == serviceName ? 0 : serviceName.length()) +
            (null == authenticator ? 0 : authenticator.length());

        if (lengthRequired > HEADER_LENGTH)
        {
            throw new IllegalArgumentException(
                "MarkFile length required " + lengthRequired + " greater than " + HEADER_LENGTH);
        }
    }
}