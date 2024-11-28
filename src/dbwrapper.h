// Copyright (c) 2012-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DBWRAPPER_H
#define BITCOIN_DBWRAPPER_H

#include <attributes.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <util/check.h>
#include <util/fs.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

static const size_t DBWRAPPER_PREALLOC_KEY_SIZE = 64;
static const size_t DBWRAPPER_PREALLOC_VALUE_SIZE = 1024;

//! User-controlled performance and debug options.
struct DBOptions {
    //! Compact database on startup.
    bool force_compact = false;
};

//! Application-specific storage settings.
struct DBParams {
    //! Location in the filesystem where leveldb data will be stored.
    fs::path path;
    //! Configures various leveldb cache settings.
    size_t cache_bytes;
    //! If true, use leveldb's memory environment.
    bool memory_only = false;
    //! If true, remove all existing data.
    bool wipe_data = false;
    //! If true, store data obfuscated via simple XOR. If false, XOR with a
    //! zero'd byte array.
    bool obfuscate = false;
    //! Passed-through options.
    DBOptions options{};
};

class dbwrapper_error : public std::runtime_error
{
public:
    explicit dbwrapper_error(const std::string& msg) : std::runtime_error(msg) {}
};

class CDBWrapperBase;

/** Batch of changes queued to be written to a CDBWrapper */
class CDBBatchBase
{
protected:
    const CDBWrapperBase &m_parent;

    DataStream ssKey{};
    DataStream ssValue{};

    virtual void WriteImpl(Span<const std::byte> key, DataStream& ssVal) = 0;
    virtual void EraseImpl(Span<const std::byte> key) = 0;

public:
    /**
     * @param[in] _parent   CDBWrapper that this batch is to be submitted to
     */
    explicit CDBBatchBase(const CDBWrapperBase& _parent) : m_parent{_parent} {}
    virtual ~CDBBatchBase() = default;
    // virtual void Clear() = 0;

    virtual size_t SizeEstimate() const  = 0;

    template <typename K, typename V>
    void Write(const K& key, const V& value)
    {
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssValue.reserve(DBWRAPPER_PREALLOC_VALUE_SIZE);
        ssKey << key;
        ssValue << value;
        WriteImpl(ssKey, ssValue);
        ssKey.clear();
        ssValue.clear();
    }

    template <typename K>
    void Erase(const K& key)
    {
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        EraseImpl(ssKey);
        ssKey.clear();
    }
};

class CDBWrapper;

/** Batch of changes queued to be written to a CDBWrapper */
class CDBBatch : public CDBBatchBase
{
    friend class CDBWrapperBase;
    friend class CDBWrapper;

private:
    struct WriteBatchImpl;
    const std::unique_ptr<WriteBatchImpl> m_impl_batch;

    size_t size_estimate{0};

    void WriteImpl(Span<const std::byte> key, DataStream& ssVal) override;
    void EraseImpl(Span<const std::byte> key) override;

public:
    /**
     * @param[in] _parent   CDBWrapper that this batch is to be submitted to
     */
    explicit CDBBatch(const CDBWrapperBase& _parent);
    ~CDBBatch() override;
    // void Clear() override;

    size_t SizeEstimate() const override { return size_estimate; }
};

class CDBIteratorBase
{
protected:
    const CDBWrapperBase &parent;

    virtual void SeekImpl(Span<const std::byte> key) = 0;
    virtual Span<const std::byte> GetKeyImpl() const = 0;
    virtual Span<const std::byte> GetValueImpl() const = 0;
public:
    explicit CDBIteratorBase(const CDBWrapperBase& _parent)
        : parent(_parent) {}
    virtual ~CDBIteratorBase() = default;


    template<typename K> void Seek(const K& key) {
        DataStream ssKey{};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        SeekImpl(ssKey);
    }

    template<typename K> bool GetKey(K& key) {
        try {
            DataStream ssKey{GetKeyImpl()};
            ssKey >> key;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    template<typename V> bool GetValue(V& value);

    virtual bool Valid() const = 0;
    virtual void SeekToFirst() = 0;
    virtual void Next() = 0;
};

class CDBIterator : public CDBIteratorBase
{
public:
    struct IteratorImpl;
private:
    const std::unique_ptr<IteratorImpl> m_impl_iter;

    void SeekImpl(Span<const std::byte> key) override;
    Span<const std::byte> GetKeyImpl() const override;
    Span<const std::byte> GetValueImpl() const override;

public:

    /**
     * @param[in] _parent          Parent CDBWrapper instance.
     * @param[in] _piter           The original leveldb iterator.
     */
    CDBIterator(const CDBWrapperBase& _parent, std::unique_ptr<IteratorImpl> _piter);
    ~CDBIterator() override;

    bool Valid() const override;
    void SeekToFirst() override;
    void Next() override;
};

class CDBWrapperBase
{
protected:
    CDBWrapperBase(const DBParams& params)
        : m_name(fs::PathToString(params.path.stem())),
          m_path(params.path),
          m_is_memory(params.memory_only)
    {
    }

    //! the name of this database
    std::string m_name;

    //! a key used for optional XOR-obfuscation of the database
    std::vector<unsigned char> obfuscate_key;

    //! the key under which the obfuscation key is stored
    static const std::string OBFUSCATE_KEY_KEY;

    //! the length of the obfuscate key in number of bytes
    static const unsigned int OBFUSCATE_KEY_NUM_BYTES;

    std::vector<unsigned char> CreateObfuscateKey() const;

    bool WriteObfuscateKeyIfNotExists();

    //! path to filesystem storage
    const fs::path m_path;

    //! whether or not the database resides in memory
    bool m_is_memory;

    virtual std::optional<std::string> ReadImpl(Span<const std::byte> key) const = 0;
    virtual bool ExistsImpl(Span<const std::byte> key) const = 0;
    virtual size_t EstimateSizeImpl(Span<const std::byte> key1, Span<const std::byte> key2) const = 0;

    virtual std::unique_ptr<CDBBatchBase> CreateBatch() const = 0;

public:
    CDBWrapperBase(const CDBWrapperBase&) = delete;
    CDBWrapperBase& operator=(const CDBWrapperBase&) = delete;

    virtual ~CDBWrapperBase() = default;

    const std::vector<unsigned char>& GetObfuscateKey() const;

    template <typename K, typename V>
    bool Read(const K& key, V& value) const
    {
        DataStream ssKey{};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        std::optional<std::string> strValue{ReadImpl(ssKey)};
        if (!strValue) {
            return false;
        }
        try {
            DataStream ssValue{MakeByteSpan(*strValue)};
            ssValue.Xor(obfuscate_key);
            ssValue >> value;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    template <typename K, typename V>
    bool Write(const K& key, const V& value, bool fSync = false)
    {
        auto batch = CreateBatch();
        batch->Write(key, value);
        return WriteBatch(*batch, fSync);
    }

    //! @returns filesystem path to the on-disk data.
    std::optional<fs::path> StoragePath() {
        if (m_is_memory) {
            return {};
        }
        return m_path;
    }

    template <typename K>
    bool Exists(const K& key) const
    {
        DataStream ssKey{};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        return ExistsImpl(ssKey);
    }

    template <typename K>
    bool Erase(const K& key, bool fSync = false)
    {
        auto batch = CreateBatch();
        batch->Erase(key);
        return WriteBatch(*batch, fSync);
    }

    virtual bool WriteBatch(CDBBatchBase& batch, bool fSync = false) = 0;

    // Get an estimate of LevelDB memory usage (in bytes).
    virtual size_t DynamicMemoryUsage() const = 0;

    virtual CDBIteratorBase* NewIterator() = 0;

    /**
     * Return true if the database managed by this class contains no entries.
     */
    virtual bool IsEmpty() = 0;

    template<typename K>
    size_t EstimateSize(const K& key_begin, const K& key_end) const
    {
        DataStream ssKey1{}, ssKey2{};
        ssKey1.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey2.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey1 << key_begin;
        ssKey2 << key_end;
        return EstimateSizeImpl(ssKey1, ssKey2);
    }
};

struct LevelDBContext;

class CDBWrapper : public CDBWrapperBase
{
private:
    //! holds all leveldb-specific fields of this class
    std::unique_ptr<LevelDBContext> m_db_context;
    auto& DBContext() const LIFETIMEBOUND { return *Assert(m_db_context); }

    std::optional<std::string> ReadImpl(Span<const std::byte> key) const override;
    bool ExistsImpl(Span<const std::byte> key) const override;
    size_t EstimateSizeImpl(Span<const std::byte> key1, Span<const std::byte> key2) const override;

    inline std::unique_ptr<CDBBatchBase> CreateBatch() const override {
        return std::make_unique<CDBBatch>(*this);
    }

    struct StatusImpl;
    static void HandleError(const CDBWrapper::StatusImpl& _status);

public:
    CDBWrapper(const DBParams& params);
    ~CDBWrapper() override;

    bool WriteBatch(CDBBatchBase& batch, bool fSync = false) override;
    size_t DynamicMemoryUsage() const override;

    CDBIteratorBase* NewIterator() override;
    bool IsEmpty() override;

    static bool DestroyDB(const std::string& path_str);
};

struct MDBXContext;

// MDBXContext is defined in mdbx.cpp to avoid dependency on libmdbx here
struct MDBXContext;

/** Batch of changes queued to be written to an MDBXWrapper */
class MDBXBatch : public CDBBatchBase
{
    // We want MDBXBatch to be able to access DBContext()
    friend class MDBXWrapper;

private:
    struct MDBXWriteBatchImpl;
    std::unique_ptr<MDBXWriteBatchImpl> m_impl_batch;

    void WriteImpl(Span<const std::byte> key, DataStream& value) override;
    void EraseImpl(Span<const std::byte> key) override;

public:
    /**
     * @param[in] _parent   CDBWrapper that this batch is to be submitted to
     */
    explicit MDBXBatch(const CDBWrapperBase& _parent);
    ~MDBXBatch();
    // void Clear() override;
    void CommitAndReset();

    size_t SizeEstimate() const override;
};

/** An iterator that maps to MDBX's cursor */
class MDBXIterator : public CDBIteratorBase
{
public:
    struct IteratorImpl;
private:
    std::unique_ptr<IteratorImpl> m_impl_iter;
    bool valid;

    void SeekImpl(Span<const std::byte> key) override;
    Span<const std::byte> GetKeyImpl() const override;
    Span<const std::byte> GetValueImpl() const override;

public:
    /**
     * @param[in] _parent          Parent CDBWrapper instance.
     * @param[in] db_context       MDBXWrapper DBContext.
     */
    MDBXIterator(const CDBWrapperBase& _parent, MDBXContext &db_context);

    ~MDBXIterator() override;

    bool Valid() const override;
    void SeekToFirst() override;
    void Next() override;
};

class MDBXWrapper : public CDBWrapperBase
{
    friend class MDBXIterator;
    friend class MDBXBatch; // We want MDBXBatch to be able to access the env and sync
                            // Is there a better mechanism than friend class?
private:
    //! holds all mdbx-specific fields of this class
    std::unique_ptr<MDBXContext> m_db_context;
    auto& DBContext() const LIFETIMEBOUND { return *Assert(m_db_context); }

    std::optional<std::string> ReadImpl(Span<const std::byte> key) const override;
    bool ExistsImpl(Span<const std::byte> key) const override;
    size_t EstimateSizeImpl(Span<const std::byte> key1, Span<const std::byte> key2) const override;

    inline std::unique_ptr<CDBBatchBase> CreateBatch() const override {
        return std::make_unique<MDBXBatch>(*this);
    }

    void Sync();

public:
    MDBXWrapper(const DBParams& params);
    ~MDBXWrapper() override;

    bool WriteBatch(CDBBatchBase& _batch, bool fSync = false) override;
    size_t DynamicMemoryUsage() const override;

    CDBIteratorBase* NewIterator() override;

    /**
     * Return true if the database managed by this class contains no entries.
     */
    bool IsEmpty() override;

    static bool DestroyDB(const std::string& path_str);
};

template<typename V> bool CDBIteratorBase::GetValue(V& value) {
    try {
        DataStream ssValue{GetValueImpl()};
        ssValue.Xor(parent.GetObfuscateKey());
        ssValue >> value;
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

#endif // BITCOIN_DBWRAPPER_H
