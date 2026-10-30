import Foundation
import Compression

/// Minimal ZIP reader (stored + deflate) for pulling ROMs out of archives.
enum ZipReader {
    struct Entry {
        let name: String
        let data: Data
    }

    enum ZipError: LocalizedError {
        case notZip, unsupported(String), corrupt
        var errorDescription: String? {
            switch self {
            case .notZip: return "Not a valid ZIP file."
            case .unsupported(let n): return "\(n) uses an unsupported compression method."
            case .corrupt: return "The ZIP file is damaged."
            }
        }
    }

    static func extract(_ zip: Data, where include: (String) -> Bool) throws -> [Entry] {
        let bytes = [UInt8](zip)
        func u16(_ o: Int) -> Int { o + 1 < bytes.count ? Int(bytes[o]) | Int(bytes[o + 1]) << 8 : 0 }
        func u32(_ o: Int) -> Int { o + 3 < bytes.count ? u16(o) | u16(o + 2) << 16 : 0 }

        // Find the end-of-central-directory record
        guard bytes.count >= 22 else { throw ZipError.notZip }
        var eocd = -1
        var i = bytes.count - 22
        let stop = max(0, bytes.count - 22 - 65_535)
        while i >= stop {
            if bytes[i] == 0x50 && bytes[i + 1] == 0x4B && bytes[i + 2] == 0x05 && bytes[i + 3] == 0x06 { eocd = i; break }
            i -= 1
        }
        guard eocd >= 0 else { throw ZipError.notZip }

        let count = u16(eocd + 10)
        var p = u32(eocd + 16)
        var out: [Entry] = []

        for _ in 0..<count {
            guard u32(p) == 0x02014B50 else { throw ZipError.corrupt }
            let method = u16(p + 10)
            let compSize = u32(p + 20)
            let size = u32(p + 24)
            let nameLen = u16(p + 28), extraLen = u16(p + 30), commentLen = u16(p + 32)
            let local = u32(p + 42)
            guard p + 46 + nameLen <= bytes.count else { throw ZipError.corrupt }
            let name = String(decoding: bytes[(p + 46)..<(p + 46 + nameLen)], as: UTF8.self)
            p += 46 + nameLen + extraLen + commentLen

            let base = (name as NSString).lastPathComponent
            if name.hasSuffix("/") || base.hasPrefix("._") || name.contains("__MACOSX") || !include(base) { continue }

            guard u32(local) == 0x04034B50 else { throw ZipError.corrupt }
            let start = local + 30 + u16(local + 26) + u16(local + 28)
            guard start + compSize <= bytes.count else { throw ZipError.corrupt }

            switch method {
            case 0:
                out.append(Entry(name: base, data: Data(bytes[start..<(start + compSize)])))
            case 8:
                var dst = [UInt8](repeating: 0, count: max(size, 1))
                let n = bytes.withUnsafeBufferPointer { src -> Int in
                    dst.withUnsafeMutableBufferPointer { d in
                        compression_decode_buffer(d.baseAddress!, size, src.baseAddress! + start, compSize, nil, COMPRESSION_ZLIB)
                    }
                }
                guard n == size else { throw ZipError.corrupt }
                out.append(Entry(name: base, data: Data(dst[0..<n])))
            default:
                throw ZipError.unsupported(base)
            }
        }
        return out
    }
}
