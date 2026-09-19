#ifndef JSC_H
#define JSC_H

/**
 * @file JSC.h
 * @brief Compression and lookup utilities for the JS8Call wordlist.
 *
 * The `JSC` utilities implement static helpers used to compress and
 * decompress strings to and from a compact bit-oriented codeword
 * representation. The header exposes small convenience types and the
 * in-memory lookup tables used by the compressor.
 * 
 * @author Jordan Sherer <keyboard@sherer.org>
 * @copyright (c) 2018 Jordan Sherer
 */

#include <QList>
#include <QMap>
#include <QPair>
#include <QStringList>
#include <QTextStream>
#include <QVector>

/**
 * @typedef CodewordPair
 * @brief Tuple containing a codeword bit-vector and an associated integer.
 *
 * This typedef aliases a `QPair<Codeword, quint32>` and is used to
 * represent a codeword together with an auxiliary integer value such as
 * the character count or an index. The first element is the
 * bit-vector representation and the second is the associated unsigned
 * integer.
 */
typedef QPair<QVector<bool>, quint32> CodewordPair; /**< Tuple(Codeword, N) where N is the number of characters */

/**
 * @typedef Codeword
 * @brief Codeword bit-vector type.
 *
 * Represents a sequence of bits composing a compressed codeword. It is
 * implemented as a `QVector<bool>` and used throughout the compressor
 * and decompressor APIs.
 */
typedef QVector<bool> Codeword; /**< Codeword bit-vector type. */

/**
 * @brief Small mapping tuple used in the static lookup arrays.
 */
typedef struct Tuple {
    char const *str; /**< Null-terminated string for the entry. */
    int size;        /**< Length in characters. */
    int index;       /**< Index into related tables. */
} Tuple;

/**
 * @class JSC
 * @brief Static compressor/lookup helper for JS8Call.
 *
 * This class provides compression and decompression helpers and static
 * lookup tables. Most methods are static and operate on QStrings or
 * plain C strings to locate indexes in the compressed dictionary.
 */
class JSC {
  public:
    /**
     * @brief Build a codeword for the given parameters.
     * @param index Index into the dictionary.
     * @param separate Whether the codeword is separated by a delimiter.
     * @param bytesize Byte-size of the codeword container.
     * @param s Additional parameter used by the encoding.
     * @param c Additional parameter used by the encoding.
     * @return A `Codeword` bit vector representing the encoded entry.
     */
    static Codeword codeword(quint32 index, bool separate, quint32 bytesize,
                             quint32 s, quint32 c);

    /**
     * @brief Compress @p text into a sequence of codewords.
     * @param text Input text to compress.
     * @return A list of `CodewordPair` entries representing the compressed data.
     */
    static QList<CodewordPair> compress(QString text);

    /**
     * @brief Decompress a codeword bit-vector into a QString.
     * @param bits Bit vector representing compressed data.
     * @return The decompressed string.
     */
    static QString decompress(Codeword const &bits);

    /**
     * @brief Test whether a word exists in the dictionary.
     * @param w Word to test.
     * @param pIndex Optional out-parameter receiving the dictionary index.
     * @return true if the word exists.
     */
    static bool exists(QString w, quint32 *pIndex);

    /**
     * @brief Lookup a word and return its dictionary index.
     * @param w Word to lookup.
     * @param ok Optional out-parameter set to true on success.
     * @return The dictionary index for the word.
     */
    static quint32 lookup(QString w, bool *ok);

    /**
     * @brief Lookup a C-string and return its dictionary index.
     * @param b Null-terminated C string to lookup.
     * @param ok Optional out-parameter set to true on success.
     * @return The dictionary index for the string.
     */
    static quint32 lookup(char const *b, bool *ok);

    static const quint32 size = 262144; /**< Size of static lookup arrays. */
    static const Tuple map[262144];     /**< Primary mapping table. */
    static const Tuple list[262144];    /**< Secondary mapping table. */

    static const quint32 prefixSize = 103; /**< Number of prefix entries. */
    static const Tuple prefix[103];        /**< Prefix lookup table. */
};

#endif // JSC_H
