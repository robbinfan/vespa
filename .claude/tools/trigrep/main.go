package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"
)

// Index format:
//
// Version 2:
//   Header:       magic(4) + version(4) + numFiles(4) + numTrigrams(4)
//   FileTable:    pathLen(2) + path
//   TrigramTable: [trigram(4) + offset(4) + count(4)] × numTrigrams
//   PostingLists: uint32 file IDs
//
// Version 3 (adds per-file mtime+size for incremental update):
//   Header:       magic(4) + version(4) + numFiles(4) + numTrigrams(4)
//   FileTable:    pathLen(2) + path + mtime(8) + size(8)
//   TrigramTable: same as v2
//   PostingLists: same as v2
//
// Version 4 (adds per-entry bloom byte for "3.5-gram" phrase pre-filtering):
//   Header:       same as v3
//   FileTable:    same as v3
//   TrigramTable: same as v3
//   PostingLists: uint32 file IDs  (same layout)
//   BloomData:    uint8 per posting entry, parallel to PostingLists
//
// The bloom byte encodes which "next-byte class" groups follow this trigram in
// the file: bit i is set if any byte in the range [i*32, i*32+31] immediately
// follows the trigram at some position. This lets -F phrase search eliminate
// candidates without reading files (no false negatives, some false positives).
//
// count == 0 in TrigramTable means the trigram was pruned (too frequent).

const magic = 0x54524749 // "TRGI"
const indexVersion = 4

// ---- posting entry ---------------------------------------------------------

// postingEntry pairs a file ID with its bloom byte for one trigram.
type postingEntry struct {
	fileID uint32
	bloom  uint8
}

// ---- trigram helpers -------------------------------------------------------

func extractTrigrams(s string) []uint32 {
	seen := make(map[uint32]bool)
	b := []byte(s)
	for i := 0; i+3 <= len(b); i++ {
		t := uint32(b[i])<<16 | uint32(b[i+1])<<8 | uint32(b[i+2])
		seen[t] = true
	}
	result := make([]uint32, 0, len(seen))
	for t := range seen {
		result = append(result, t)
	}
	return result
}

// extractLiterals pulls required literal substrings from a regex pattern.
// Returns up to 3 of the longest runs so we can derive their trigrams.
func extractLiterals(pattern string) []string {
	var literals []string
	var current strings.Builder
	escaped := false
	inClass := false

	flush := func() {
		s := current.String()
		if len(s) >= 3 {
			literals = append(literals, s)
		}
		current.Reset()
	}

	for i := 0; i < len(pattern); i++ {
		c := pattern[i]
		if escaped {
			escaped = false
			switch c {
			case 'd', 'w', 's', 'D', 'W', 'S', 'b', 'B', 'n', 't', 'r':
				flush()
			default:
				current.WriteByte(c)
			}
			continue
		}
		if c == '\\' {
			escaped = true
			continue
		}
		if inClass {
			if c == ']' {
				inClass = false
				flush()
			}
			continue
		}
		switch c {
		case '[':
			flush()
			inClass = true
		case '.', '*', '+', '?', '(', ')', '|', '^', '$', '{', '}':
			flush()
		default:
			current.WriteByte(c)
		}
	}
	flush()

	sort.Slice(literals, func(i, j int) bool { return len(literals[i]) > len(literals[j]) })
	if len(literals) > 3 {
		literals = literals[:3]
	}
	return literals
}

// ---- file collection -------------------------------------------------------

type fileInfo struct {
	path  string // relative path
	mtime int64
	size  int64
}

func collectFiles(dir, exts, ignore string) ([]fileInfo, error) {
	extSet := make(map[string]bool)
	for _, e := range strings.Split(exts, ",") {
		e = strings.TrimSpace(e)
		if e != "" {
			extSet["."+e] = true
		}
	}
	ignoreSet := strings.Split(ignore, ",")
	for i := range ignoreSet {
		ignoreSet[i] = strings.TrimSpace(ignoreSet[i])
	}

	var files []fileInfo
	err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return nil
		}
		if info.IsDir() {
			name := info.Name()
			if name == ".git" || name == ".claude" {
				return filepath.SkipDir
			}
			for _, ig := range ignoreSet {
				if ig != "" && strings.Contains(path, ig) {
					return filepath.SkipDir
				}
			}
			return nil
		}
		if len(extSet) > 0 && !extSet[filepath.Ext(path)] {
			return nil
		}
		for _, ig := range ignoreSet {
			if ig != "" && strings.Contains(path, ig) {
				return nil
			}
		}
		rel, err := filepath.Rel(dir, path)
		if err != nil {
			return nil
		}
		files = append(files, fileInfo{
			path:  rel,
			mtime: info.ModTime().Unix(),
			size:  info.Size(),
		})
		return nil
	})
	return files, err
}

// extractTrigramsFromFile reads a file and returns the set of unique trigrams
// with their bloom byte. The bloom byte records which "next-byte class" groups
// (bit i = chars [i*32, i*32+31]) follow the trigram somewhere in this file.
func extractTrigramsFromFile(absPath string) map[uint32]uint8 {
	f, err := os.Open(absPath)
	if err != nil {
		return nil
	}
	defer f.Close()
	content, err := io.ReadAll(io.LimitReader(f, 1<<20))
	if err != nil {
		return nil
	}
	bloom := make(map[uint32]uint8)
	for i := 0; i+3 <= len(content); i++ {
		t := uint32(content[i])<<16 | uint32(content[i+1])<<8 | uint32(content[i+2])
		bloom[t] |= 0 // ensure key exists even with no next byte
		if i+3 < len(content) {
			bloom[t] |= 1 << (content[i+3] >> 5)
		}
	}
	return bloom
}

// ---- build (full, parallel) ------------------------------------------------

type buildCmd struct {
	dir     string
	output  string
	exts    string
	ignore  string
	maxFreq float64
}

type fileResult struct {
	fileID  int
	trigrams map[uint32]uint8
}

func (b *buildCmd) run() error {
	start := time.Now()

	files, err := collectFiles(b.dir, b.exts, b.ignore)
	if err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "Indexing %d files...\n", len(files))

	// Parallel file reading + trigram extraction.
	// Launcher runs in its own goroutine so the main goroutine can drain
	// results immediately — avoids deadlock when results channel fills up.
	results := make(chan fileResult, runtime.NumCPU()*4)
	sem := make(chan struct{}, runtime.NumCPU())
	var wg sync.WaitGroup
	go func() {
		for i, fi := range files {
			wg.Add(1)
			sem <- struct{}{}
			go func(i int, fi fileInfo) {
				defer wg.Done()
				defer func() { <-sem }()
				tgrams := extractTrigramsFromFile(filepath.Join(b.dir, fi.path))
				results <- fileResult{i, tgrams}
			}(i, fi)
		}
		wg.Wait()
		close(results)
	}()

	// Collect all results, then sort by fileID so posting lists stay naturally
	// ordered — avoids per-list sort of 13M+ entries.
	allResults := make([]fileResult, 0, len(files))
	for r := range results {
		allResults = append(allResults, r)
	}
	sort.Slice(allResults, func(i, j int) bool {
		return allResults[i].fileID < allResults[j].fileID
	})

	posting := make(map[uint32][]postingEntry)
	for i, r := range allResults {
		for t, bl := range r.trigrams {
			posting[t] = append(posting[t], postingEntry{uint32(r.fileID), bl})
		}
		if (i+1)%2000 == 0 {
			fmt.Fprintf(os.Stderr, "  %d/%d files processed\n", i+1, len(files))
		}
	}

	return writeIndex(b.output, files, posting, b.maxFreq, start)
}

// ---- update (incremental) --------------------------------------------------

type updateCmd struct {
	indexPath string
	dir       string
	exts      string
	ignore    string
	maxFreq   float64
}

func (u *updateCmd) run() error {
	start := time.Now()

	idx, err := loadIndex(u.indexPath, false)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Cannot load existing index (%v), falling back to full build.\n", err)
		b := &buildCmd{dir: u.dir, output: u.indexPath, exts: u.exts, ignore: u.ignore, maxFreq: u.maxFreq}
		return b.run()
	}
	if idx.version < 3 {
		fmt.Fprintf(os.Stderr, "Index is version %d (no mtime data), falling back to full build.\n", idx.version)
		b := &buildCmd{dir: u.dir, output: u.indexPath, exts: u.exts, ignore: u.ignore, maxFreq: u.maxFreq}
		return b.run()
	}

	newFiles, err := collectFiles(u.dir, u.exts, u.ignore)
	if err != nil {
		return err
	}

	type oldMeta struct {
		idx   int
		mtime int64
		size  int64
	}
	oldByPath := make(map[string]oldMeta, len(idx.files))
	for i, fi := range idx.files {
		oldByPath[fi.path] = oldMeta{i, fi.mtime, fi.size}
	}

	unchangedOldID := make([]int, len(newFiles))
	for i := range unchangedOldID {
		unchangedOldID[i] = -1
	}
	added, modified := 0, 0
	for newFID, fi := range newFiles {
		om, ok := oldByPath[fi.path]
		if ok && om.mtime == fi.mtime && om.size == fi.size {
			unchangedOldID[newFID] = om.idx
		} else if ok {
			modified++
		} else {
			added++
		}
	}
	newPathSet := make(map[string]bool, len(newFiles))
	for _, fi := range newFiles {
		newPathSet[fi.path] = true
	}
	deleted := 0
	for _, fi := range idx.files {
		if !newPathSet[fi.path] {
			deleted++
		}
	}
	unchanged := len(newFiles) - modified - added
	fmt.Fprintf(os.Stderr, "Files: %d total, %d unchanged, %d modified, %d added, %d deleted\n",
		len(newFiles), unchanged, modified, added, deleted)

	if unchanged < len(newFiles)/2 {
		fmt.Fprintf(os.Stderr, "Too many changes, doing full rebuild.\n")
		b := &buildCmd{dir: u.dir, output: u.indexPath, exts: u.exts, ignore: u.ignore, maxFreq: u.maxFreq}
		return b.run()
	}

	// Invert posting lists into per-oldFile (trigram, bloom) slices.
	// For v3 indexes (no bloom), use 0xFF (all bits set) as conservative placeholder.
	hasBloom := idx.version >= 4 && len(idx.bloomData) > 0

	type trigramBloom struct {
		trigram uint32
		bloom   uint8
	}
	perFile := make([][]trigramBloom, len(idx.files))
	for newFID, oldID := range unchangedOldID {
		if oldID >= 0 {
			_ = newFID
			if perFile[oldID] == nil {
				perFile[oldID] = make([]trigramBloom, 0, 128)
			}
		}
	}

	// Pruned trigrams: not stored in posting lists; add to all unchanged files
	// as 0xFF bloom so writeIndex re-prunes them correctly.
	var prunedEntries []trigramBloom
	data := idx.data
	for ti, t := range idx.trigrams {
		cnt := idx.counts[ti]
		if cnt == 0 {
			prunedEntries = append(prunedEntries, trigramBloom{t, 0xFF})
			continue
		}
		off := idx.offsets[ti]
		relBase := (off - idx.postingBase) / 4
		for i := uint32(0); i < cnt; i++ {
			fid := binary.LittleEndian.Uint32(data[off+i*4:])
			if perFile[fid] != nil {
				var bl uint8
				if hasBloom {
					bl = idx.bloomData[relBase+i]
				} else {
					bl = 0xFF
				}
				perFile[fid] = append(perFile[fid], trigramBloom{t, bl})
			}
		}
	}
	for oldID, pf := range perFile {
		if pf != nil {
			perFile[oldID] = append(pf, prunedEntries...)
		}
	}

	// Build new posting map.
	posting := make(map[uint32][]postingEntry)
	for newFID, fi := range newFiles {
		oldID := unchangedOldID[newFID]
		if oldID >= 0 {
			for _, tb := range perFile[oldID] {
				posting[tb.trigram] = append(posting[tb.trigram], postingEntry{uint32(newFID), tb.bloom})
			}
		} else {
			for t, bl := range extractTrigramsFromFile(filepath.Join(u.dir, fi.path)) {
				posting[t] = append(posting[t], postingEntry{uint32(newFID), bl})
			}
		}
	}

	return writeIndex(u.indexPath, newFiles, posting, u.maxFreq, start)
}

// ---- index write -----------------------------------------------------------

func writeIndex(output string, files []fileInfo, posting map[uint32][]postingEntry, maxFreq float64, start time.Time) error {
	pruneThreshold := 0
	if maxFreq > 0 && maxFreq < 1.0 {
		pruneThreshold = int(maxFreq * float64(len(files)))
	}

	trigrams := make([]uint32, 0, len(posting))
	for t := range posting {
		trigrams = append(trigrams, t)
	}
	sort.Slice(trigrams, func(i, j int) bool { return trigrams[i] < trigrams[j] })

	pruned := make(map[uint32]bool)
	if pruneThreshold > 0 {
		for _, t := range trigrams {
			if len(posting[t]) > pruneThreshold {
				pruned[t] = true
			}
		}
	}

	if err := os.MkdirAll(filepath.Dir(output), 0755); err != nil {
		return err
	}
	tmp := output + ".tmp"
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	w := bufio.NewWriterSize(out, 1<<20)

	write32 := func(v uint32) {
		var buf [4]byte
		binary.LittleEndian.PutUint32(buf[:], v)
		w.Write(buf[:])
	}
	write16 := func(v uint16) {
		var buf [2]byte
		binary.LittleEndian.PutUint16(buf[:], v)
		w.Write(buf[:])
	}
	write64 := func(v int64) {
		var buf [8]byte
		binary.LittleEndian.PutUint64(buf[:], uint64(v))
		w.Write(buf[:])
	}

	// Header
	write32(magic)
	write32(indexVersion) // v4
	write32(uint32(len(files)))
	write32(uint32(len(trigrams)))

	// File table
	for _, fi := range files {
		write16(uint16(len(fi.path)))
		w.WriteString(fi.path)
		write64(fi.mtime)
		write64(fi.size)
	}

	// Compute postingStart
	headerSize := 16
	fileTableSize := 0
	for _, fi := range files {
		fileTableSize += 2 + len(fi.path) + 16
	}
	trigramTableSize := len(trigrams) * 12
	postingStart := uint32(headerSize + fileTableSize + trigramTableSize)

	// Trigram table
	offset := postingStart
	for _, t := range trigrams {
		cnt := uint32(len(posting[t]))
		write32(t)
		if pruned[t] {
			write32(0)
			write32(0)
		} else {
			write32(offset)
			write32(cnt)
			offset += cnt * 4
		}
	}

	// Posting lists (fileIDs, non-pruned only)
	for _, t := range trigrams {
		if pruned[t] {
			continue
		}
		for _, e := range posting[t] {
			write32(e.fileID)
		}
	}

	// Bloom section (1 byte per posting entry, parallel to posting lists)
	for _, t := range trigrams {
		if pruned[t] {
			continue
		}
		for _, e := range posting[t] {
			w.WriteByte(e.bloom)
		}
	}

	if err := w.Flush(); err != nil {
		out.Close()
		os.Remove(tmp)
		return err
	}
	out.Close()
	if err := os.Rename(tmp, output); err != nil {
		os.Remove(tmp)
		return err
	}

	elapsed := time.Since(start).Round(time.Millisecond)
	if pruneThreshold > 0 {
		fmt.Fprintf(os.Stderr,
			"Index written in %v: %d files, %d trigrams (%d pruned, freq>%.0f%%) -> %s\n",
			elapsed, len(files), len(trigrams), len(pruned), maxFreq*100, output)
	} else {
		fmt.Fprintf(os.Stderr,
			"Index written in %v: %d files, %d unique trigrams -> %s\n",
			elapsed, len(files), len(trigrams), output)
	}
	return nil
}

// ---- index load ------------------------------------------------------------

type index struct {
	version     uint32
	files       []fileInfo
	trigrams    []uint32 // sorted
	offsets     []uint32
	counts      []uint32 // count==0 means pruned
	data        []byte
	postingBase uint32 // byte offset of start of posting lists in data
	bloomData   []byte // nil for v3 and earlier
}

func loadIndex(path string, useMmap bool) (*index, error) {
	var data []byte
	if useMmap {
		f, err := os.Open(path)
		if err != nil {
			return nil, err
		}
		fi, err := f.Stat()
		if err != nil {
			f.Close()
			return nil, err
		}
		size := int(fi.Size())
		if size == 0 {
			f.Close()
			return nil, fmt.Errorf("index is empty")
		}
		mapped, err := syscall.Mmap(int(f.Fd()), 0, size, syscall.PROT_READ, syscall.MAP_SHARED)
		f.Close() // fd can be closed after mmap; mapping stays valid
		if err != nil {
			return nil, fmt.Errorf("mmap: %w", err)
		}
		data = mapped
	} else {
		var err error
		data, err = os.ReadFile(path)
		if err != nil {
			return nil, err
		}
	}

	if len(data) < 16 {
		return nil, fmt.Errorf("index too small")
	}
	if binary.LittleEndian.Uint32(data[0:]) != magic {
		return nil, fmt.Errorf("invalid index magic")
	}
	ver := binary.LittleEndian.Uint32(data[4:])
	numFiles := int(binary.LittleEndian.Uint32(data[8:]))
	numTrigrams := int(binary.LittleEndian.Uint32(data[12:]))

	pos := 16
	files := make([]fileInfo, numFiles)
	for i := range files {
		if pos+2 > len(data) {
			return nil, fmt.Errorf("index corrupt at file table")
		}
		l := int(binary.LittleEndian.Uint16(data[pos:]))
		pos += 2
		if pos+l > len(data) {
			return nil, fmt.Errorf("index corrupt at file path")
		}
		files[i].path = string(data[pos : pos+l])
		pos += l
		if ver >= 3 {
			if pos+16 > len(data) {
				return nil, fmt.Errorf("index corrupt at file mtime/size")
			}
			files[i].mtime = int64(binary.LittleEndian.Uint64(data[pos:]))
			files[i].size = int64(binary.LittleEndian.Uint64(data[pos+8:]))
			pos += 16
		}
	}

	trigrams := make([]uint32, numTrigrams)
	offsets := make([]uint32, numTrigrams)
	counts := make([]uint32, numTrigrams)
	for i := 0; i < numTrigrams; i++ {
		if pos+12 > len(data) {
			return nil, fmt.Errorf("index corrupt at trigram table")
		}
		trigrams[i] = binary.LittleEndian.Uint32(data[pos:])
		offsets[i] = binary.LittleEndian.Uint32(data[pos+4:])
		counts[i] = binary.LittleEndian.Uint32(data[pos+8:])
		pos += 12
	}

	postingBase := uint32(pos)

	// Compute total posting entries to locate bloom section.
	var totalEntries uint32
	for _, cnt := range counts {
		totalEntries += cnt
	}

	idx := &index{
		version:     ver,
		files:       files,
		trigrams:    trigrams,
		offsets:     offsets,
		counts:      counts,
		data:        data,
		postingBase: postingBase,
	}

	// Parse bloom section (v4+).
	if ver >= 4 {
		bloomStart := int(postingBase) + int(totalEntries)*4
		bloomEnd := bloomStart + int(totalEntries)
		if bloomEnd <= len(data) {
			idx.bloomData = data[bloomStart:bloomEnd]
		}
	}

	return idx, nil
}

// findTrigram returns the table index of trigram t, or -1 if not found.
func (idx *index) findTrigram(t uint32) int {
	lo, hi := 0, len(idx.trigrams)
	for lo < hi {
		mid := (lo + hi) / 2
		if idx.trigrams[mid] < t {
			lo = mid + 1
		} else {
			hi = mid
		}
	}
	if lo >= len(idx.trigrams) || idx.trigrams[lo] != t {
		return -1
	}
	return lo
}

// postingList returns file IDs for trigram at table index ti.
func (idx *index) postingList(ti int) []uint32 {
	cnt := idx.counts[ti]
	if cnt == 0 {
		return nil
	}
	off := idx.offsets[ti]
	result := make([]uint32, cnt)
	for i := uint32(0); i < cnt; i++ {
		result[i] = binary.LittleEndian.Uint32(idx.data[off+i*4:])
	}
	return result
}

// lookup returns (list, pruned) for trigram value t.
func (idx *index) lookup(t uint32) ([]uint32, bool) {
	ti := idx.findTrigram(t)
	if ti < 0 {
		return nil, false
	}
	if idx.counts[ti] == 0 {
		return nil, true
	}
	return idx.postingList(ti), false
}

func intersect(a, b []uint32) []uint32 {
	var result []uint32
	i, j := 0, 0
	for i < len(a) && j < len(b) {
		if a[i] == b[j] {
			result = append(result, a[i])
			i++
			j++
		} else if a[i] < b[j] {
			i++
		} else {
			j++
		}
	}
	return result
}

// bloomFilter applies the bloom pre-filter to candidates using phrase trigrams.
// For each trigram at position i in phrase where phrase[i+3] is known,
// eliminates candidates whose stored bloom byte doesn't include the expected
// next-byte class. Zero false negatives; some false positives are expected.
//
// Bloom byte encoding: bit k is set if any byte in [k*32, k*32+31] follows
// the trigram in this file. nextBit = 1 << (phrase[i+3] >> 5).
func (idx *index) bloomFilter(candidates []uint32, phrase []byte) []uint32 {
	if len(idx.bloomData) == 0 || len(phrase) < 4 {
		return candidates
	}
	for i := 0; i+4 <= len(phrase); i++ {
		t := uint32(phrase[i])<<16 | uint32(phrase[i+1])<<8 | uint32(phrase[i+2])
		nextBit := uint8(1 << (phrase[i+3] >> 5))

		ti := idx.findTrigram(t)
		if ti < 0 || idx.counts[ti] == 0 {
			continue
		}

		off := idx.offsets[ti]
		cnt := idx.counts[ti]
		relBase := (off - idx.postingBase) / 4

		// Merge-scan: both candidates and posting list are sorted by fileID.
		filtered := candidates[:0]
		ci, pi := 0, uint32(0)
		for ci < len(candidates) && pi < cnt {
			fid := binary.LittleEndian.Uint32(idx.data[off+pi*4:])
			c := candidates[ci]
			switch {
			case fid < c:
				pi++
			case fid == c:
				if idx.bloomData[relBase+pi]&nextBit != 0 {
					filtered = append(filtered, c)
				}
				ci++
				pi++
			default: // fid > c: candidate not in this posting (conservative: keep)
				filtered = append(filtered, c)
				ci++
			}
		}
		// Any remaining candidates past end of posting list: keep conservatively.
		filtered = append(filtered, candidates[ci:]...)
		candidates = filtered
		if len(candidates) == 0 {
			break
		}
	}
	return candidates
}

// ---- search ----------------------------------------------------------------

type searchCmd struct {
	indexPath string
	pattern   string
	literal   bool
	filesOnly bool
	context   int
	rootDir   string
	useMmap   bool
}

func (s *searchCmd) run() error {
	idx, err := loadIndex(s.indexPath, s.useMmap)
	if err != nil {
		return fmt.Errorf("loading index: %w", err)
	}

	if s.literal {
		return s.runLiteral(idx)
	}

	re, err := regexp.Compile(s.pattern)
	if err != nil {
		return fmt.Errorf("invalid pattern: %w", err)
	}

	candidates := s.candidatesFromLiterals(idx, extractLiterals(s.pattern))
	fmt.Fprintf(os.Stderr, "Candidates: %d / %d files\n", len(candidates), len(idx.files))

	rootDir := s.rootDir
	if rootDir == "" {
		rootDir = "."
	}

	for _, fid := range candidates {
		fpath := filepath.Join(rootDir, idx.files[fid].path)
		f, err := os.Open(fpath)
		if err != nil {
			continue
		}

		if s.filesOnly {
			content, err := io.ReadAll(f)
			f.Close()
			if err != nil {
				continue
			}
			if re.Match(content) {
				fmt.Println(idx.files[fid].path)
			}
		} else {
			scanner := bufio.NewScanner(f)
			var lines []string
			for scanner.Scan() {
				lines = append(lines, scanner.Text())
			}
			f.Close()

			printed := false
			for lineNo, line := range lines {
				if re.MatchString(line) {
					if !printed {
						fmt.Printf("\n%s\n", idx.files[fid].path)
						printed = true
					}
					start := lineNo - s.context
					if start < 0 {
						start = 0
					}
					end := lineNo + s.context + 1
					if end > len(lines) {
						end = len(lines)
					}
					for i := start; i < end; i++ {
						if i == lineNo {
							fmt.Printf("%d: %s\n", i+1, lines[i])
						} else {
							fmt.Printf("%d- %s\n", i+1, lines[i])
						}
					}
					fmt.Println("--")
				}
			}
		}
	}

	return nil
}

// runLiteral handles -F (fixed string) search with bloom pre-filter.
func (s *searchCmd) runLiteral(idx *index) error {
	needle := []byte(s.pattern)

	candidates := s.candidatesFromLiterals(idx, []string{s.pattern})

	// Bloom pre-filter: eliminate candidates where the phrase's trigram→next-char
	// class bits don't match — no disk reads needed for eliminated files.
	before := len(candidates)
	candidates = idx.bloomFilter(candidates, needle)
	if len(idx.bloomData) > 0 {
		fmt.Fprintf(os.Stderr, "Candidates: %d / %d files (bloom filtered %d)\n",
			len(candidates), len(idx.files), before-len(candidates))
	} else {
		fmt.Fprintf(os.Stderr, "Candidates: %d / %d files\n", len(candidates), len(idx.files))
	}

	rootDir := s.rootDir
	if rootDir == "" {
		rootDir = "."
	}

	for _, fid := range candidates {
		fpath := filepath.Join(rootDir, idx.files[fid].path)
		content, err := os.ReadFile(fpath)
		if err != nil {
			continue
		}

		if s.filesOnly {
			if bytes.Contains(content, needle) {
				fmt.Println(idx.files[fid].path)
			}
		} else {
			lines := strings.Split(string(content), "\n")
			printed := false
			for lineNo, line := range lines {
				if strings.Contains(line, s.pattern) {
					if !printed {
						fmt.Printf("\n%s\n", idx.files[fid].path)
						printed = true
					}
					start := lineNo - s.context
					if start < 0 {
						start = 0
					}
					end := lineNo + s.context + 1
					if end > len(lines) {
						end = len(lines)
					}
					for i := start; i < end; i++ {
						if i == lineNo {
							fmt.Printf("%d: %s\n", i+1, lines[i])
						} else {
							fmt.Printf("%d- %s\n", i+1, lines[i])
						}
					}
					fmt.Println("--")
				}
			}
		}
	}
	return nil
}

// candidatesFromLiterals intersects posting lists for trigrams derived from
// the given literal strings. Returns all file IDs if no usable trigrams exist.
func (s *searchCmd) candidatesFromLiterals(idx *index, literals []string) []uint32 {
	allFiles := func() []uint32 {
		c := make([]uint32, len(idx.files))
		for i := range c {
			c[i] = uint32(i)
		}
		return c
	}

	var candidates []uint32
	for li, lit := range literals {
		ts := extractTrigrams(lit)
		if len(ts) == 0 {
			continue
		}

		var litCandidates []uint32
		initialized := false
		allPruned := true

		for _, t := range ts {
			posts, pruned := idx.lookup(t)
			if pruned {
				continue
			}
			allPruned = false
			if posts == nil {
				litCandidates = nil
				initialized = true
				break
			}
			if !initialized {
				litCandidates = posts
				initialized = true
			} else {
				litCandidates = intersect(litCandidates, posts)
			}
			if len(litCandidates) == 0 {
				break
			}
		}

		if allPruned {
			continue
		}

		if li == 0 || candidates == nil {
			candidates = litCandidates
		} else {
			candidates = intersect(candidates, litCandidates)
		}
		if len(candidates) == 0 {
			break
		}
	}

	if candidates == nil {
		return allFiles()
	}
	return candidates
}

// ---- main ------------------------------------------------------------------

func main() {
	buildFlags := flag.NewFlagSet("build", flag.ExitOnError)
	buildDir := buildFlags.String("dir", ".", "directory to index")
	buildOutput := buildFlags.String("output", ".claude/trigram-index.bin", "output index file")
	buildExts := buildFlags.String("ext", "cpp,h,java,py,go,rs,ts,js,c,cc,hpp,hh", "file extensions to index")
	buildIgnore := buildFlags.String("ignore", "vendor,node_modules,target,build", "directory patterns to ignore")
	buildMaxFreq := buildFlags.Float64("max-freq", 0.5, "prune trigrams appearing in >N fraction of files (0=disabled)")

	updateFlags := flag.NewFlagSet("update", flag.ExitOnError)
	updateIndex := updateFlags.String("index", ".claude/trigram-index.bin", "existing index to update")
	updateDir := updateFlags.String("dir", ".", "directory to index")
	updateExts := updateFlags.String("ext", "cpp,h,java,py,go,rs,ts,js,c,cc,hpp,hh", "file extensions")
	updateIgnore := updateFlags.String("ignore", "vendor,node_modules,target,build", "directory patterns to ignore")
	updateMaxFreq := updateFlags.Float64("max-freq", 0.5, "prune threshold")

	searchFlags := flag.NewFlagSet("search", flag.ExitOnError)
	searchIndex := searchFlags.String("index", ".claude/trigram-index.bin", "index file")
	searchPattern := searchFlags.String("pattern", "", "regex pattern (or literal with -F)")
	searchLiteral := searchFlags.Bool("literal", false, "treat pattern as fixed string, not regex (-F shorthand also accepted)")
	searchFlags.BoolVar(searchLiteral, "F", false, "shorthand for --literal")
	searchFilesOnly := searchFlags.Bool("files-only", false, "only print matching file paths")
	searchContext := searchFlags.Int("context", 2, "lines of context")
	searchRoot := searchFlags.String("root", ".", "root directory for resolving file paths")
	searchMmap := searchFlags.Bool("mmap", false, "use mmap to load index (lower cold-start latency, avoids 44MB copy)")

	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "Usage: trigrep <build|update|search> [options]")
		os.Exit(1)
	}

	switch os.Args[1] {
	case "build":
		buildFlags.Parse(os.Args[2:])
		cmd := &buildCmd{
			dir:     *buildDir,
			output:  *buildOutput,
			exts:    *buildExts,
			ignore:  *buildIgnore,
			maxFreq: *buildMaxFreq,
		}
		if err := cmd.run(); err != nil {
			fmt.Fprintln(os.Stderr, "Error:", err)
			os.Exit(1)
		}
	case "update":
		updateFlags.Parse(os.Args[2:])
		cmd := &updateCmd{
			indexPath: *updateIndex,
			dir:       *updateDir,
			exts:      *updateExts,
			ignore:    *updateIgnore,
			maxFreq:   *updateMaxFreq,
		}
		if err := cmd.run(); err != nil {
			fmt.Fprintln(os.Stderr, "Error:", err)
			os.Exit(1)
		}
	case "search":
		searchFlags.Parse(os.Args[2:])
		if *searchPattern == "" {
			fmt.Fprintln(os.Stderr, "Error: --pattern required")
			os.Exit(1)
		}
		cmd := &searchCmd{
			indexPath: *searchIndex,
			pattern:   *searchPattern,
			literal:   *searchLiteral,
			filesOnly: *searchFilesOnly,
			context:   *searchContext,
			rootDir:   *searchRoot,
			useMmap:   *searchMmap,
		}
		if err := cmd.run(); err != nil {
			fmt.Fprintln(os.Stderr, "Error:", err)
			os.Exit(1)
		}
	default:
		fmt.Fprintln(os.Stderr, "Unknown command:", os.Args[1])
		os.Exit(1)
	}
}
