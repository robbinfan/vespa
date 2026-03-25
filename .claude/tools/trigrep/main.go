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
	"regexp/syntax"
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
// Version 4 (adds 8-bit bloom filter for phrase-aware search):
//   Header:       same as v3
//   FileTable:    same as v3
//   TrigramTable: same as v3
//   PostingLists: uint32 file IDs  (same byte offsets as v3)
//   BloomData:    uint8 per posting entry, parallel to PostingLists
//                 bloom byte = OR of (1 << (nextChar >> 5)) for each
//                 occurrence of the trigram in the file. Encodes which of
//                 8 character-class groups (each 32 chars) can follow the
//                 trigram — enables "3.5-gram" filtering without storing
//                 full positional data.
//
// count == 0 in TrigramTable means the trigram was pruned (too frequent).

const magic = 0x54524749 // "TRGI"
const version = 4

// postingEntry pairs a file ID with its bloom byte.
// bloom bit i is set if any byte in [i*32, i*32+31] follows this trigram in the file.
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

// ---- regexp → trigram plan -------------------------------------------------
//
// Instead of extracting flat literal substrings (which incorrectly ANDs all
// literals together), we build an AND/OR tree that mirrors the regexp structure:
//
//   Literal  →  planLits (all trigrams required)
//   Concat   →  planAnd  (all children required)
//   Alternate→  planOr   (at least one child required)
//   Plus     →  same as child (at least one occurrence)
//   Star/Quest/CharClass/Dot → nil (unconstrained)
//
// Example: "(void|bool).*run\(" → And[Or[lits("void"), lits("bool")], lits("run(")]
// This correctly unions "void" and "bool" candidates before intersecting with "run(".
// The old extractLiterals approach would AND all three, wrongly excluding
// files that have "void run(" but not "bool".

type trigramPlan interface{ isTrigram() }

type planAnd  struct{ children []trigramPlan }
type planOr   struct{ children []trigramPlan }
type planLits struct{ trigrams []uint32 }

func (planAnd)  isTrigram() {}
func (planOr)   isTrigram() {}
func (planLits) isTrigram() {}

// regexpToPlan converts a parsed regexp to a trigramPlan.
// nil means "no constraint" (might match all files).
func regexpToPlan(re *syntax.Regexp) trigramPlan {
	switch re.Op {
	case syntax.OpLiteral:
		if re.Flags&syntax.FoldCase != 0 {
			// Case-insensitive literal: any case variant may appear in the file.
			// Since the index is case-sensitive we can't constrain — be conservative.
			return nil
		}
		b := []byte(string(re.Rune))
		ts := extractTrigrams(string(b))
		if len(ts) == 0 {
			return nil
		}
		return planLits{ts}

	case syntax.OpConcat:
		var children []trigramPlan
		for _, sub := range re.Sub {
			if p := regexpToPlan(sub); p != nil {
				children = append(children, p)
			}
		}
		switch len(children) {
		case 0:
			return nil
		case 1:
			return children[0]
		default:
			return planAnd{children}
		}

	case syntax.OpAlternate:
		children := make([]trigramPlan, 0, len(re.Sub))
		for _, sub := range re.Sub {
			p := regexpToPlan(sub)
			if p == nil {
				// One branch unconstrained → whole OR is unconstrained.
				return nil
			}
			children = append(children, p)
		}
		switch len(children) {
		case 0:
			return nil
		case 1:
			return children[0]
		default:
			return planOr{children}
		}

	case syntax.OpCapture:
		return regexpToPlan(re.Sub[0])

	case syntax.OpPlus:
		// At least one occurrence: child's constraint applies.
		return regexpToPlan(re.Sub[0])

	default:
		// OpStar, OpQuest, OpRepeat with min=0, OpDot, OpCharClass, etc.
		// May match zero times or arbitrary chars: no trigram constraint.
		return nil
	}
}

// evalPlan evaluates a trigramPlan against the index.
// Returns (candidates, unconstrained). If unconstrained is true, all files qualify.
func evalPlan(idx *index, plan trigramPlan) ([]uint32, bool) {
	if plan == nil {
		return nil, true
	}
	switch p := plan.(type) {

	case planLits:
		// Intersect posting lists for all required trigrams.
		var cands []uint32
		constrained := false
		for _, t := range p.trigrams {
			posts, pruned := idx.lookup(t)
			if pruned {
				continue // too common, skip constraint
			}
			if posts == nil {
				return nil, false // trigram absent → impossible
			}
			if !constrained {
				cands = posts
				constrained = true
			} else {
				cands = intersect(cands, posts)
				if len(cands) == 0 {
					return nil, false
				}
			}
		}
		if !constrained {
			return nil, true // all trigrams pruned
		}
		return cands, false

	case planAnd:
		var cands []uint32
		constrained := false
		for _, child := range p.children {
			childCands, childFree := evalPlan(idx, child)
			if childFree {
				continue
			}
			if childCands == nil {
				return nil, false
			}
			if !constrained {
				cands = childCands
				constrained = true
			} else {
				cands = intersect(cands, childCands)
				if len(cands) == 0 {
					return nil, false
				}
			}
		}
		if !constrained {
			return nil, true
		}
		return cands, false

	case planOr:
		var cands []uint32
		for _, child := range p.children {
			childCands, childFree := evalPlan(idx, child)
			if childFree {
				return nil, true // one branch unconstrained → all files
			}
			if childCands == nil {
				continue // this branch matches nothing
			}
			cands = union(cands, childCands)
		}
		return cands, len(cands) == 0 // empty OR result → either impossible or all pruned
	}
	return nil, true
}

// ---- file collection -------------------------------------------------------

type fileInfo struct {
	path  string
	mtime int64
	size  int64
}

// hasIgnoredComponent reports whether any slash-separated component of path
// exactly matches one of the ignore patterns. This prevents false matches like
// "target" matching "summarycompacttarget.cpp".
func hasIgnoredComponent(path string, ignoreSet []string) bool {
	for _, part := range strings.Split(filepath.ToSlash(path), "/") {
		for _, ig := range ignoreSet {
			if ig != "" && part == ig {
				return true
			}
		}
	}
	return false
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
			if hasIgnoredComponent(path, ignoreSet) {
				return filepath.SkipDir
			}
			return nil
		}
		if len(extSet) > 0 && !extSet[filepath.Ext(path)] {
			return nil
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

// extractTrigramsFromFile reads a file and returns a map of trigram → bloom byte.
// The bloom byte records which character-class groups (each covering 32 chars)
// appear immediately after the trigram: bit i = 1 << (nextByte >> 5).
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
	bloom := make(map[uint32]uint8, len(content)/4)
	for i := 0; i+3 <= len(content); i++ {
		t := uint32(content[i])<<16 | uint32(content[i+1])<<8 | uint32(content[i+2])
		if i+3 < len(content) {
			bloom[t] |= 1 << (content[i+3] >> 5)
		} else {
			if _, ok := bloom[t]; !ok {
				bloom[t] = 0
			}
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

func (b *buildCmd) run() error {
	start := time.Now()

	files, err := collectFiles(b.dir, b.exts, b.ignore)
	if err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "Indexing %d files with %d workers...\n", len(files), runtime.NumCPU())

	type fileResult struct {
		fileID  int
		trigrams map[uint32]uint8
	}

	results := make(chan fileResult, runtime.NumCPU()*4)
	sem := make(chan struct{}, runtime.NumCPU())
	var wg sync.WaitGroup

	// Run launcher in its own goroutine: sem blocks when all workers are busy,
	// so the main goroutine must be free to drain results — otherwise deadlock.
	go func() {
		for i, fi := range files {
			wg.Add(1)
			sem <- struct{}{}
			go func(i int, fi fileInfo) {
				defer wg.Done()
				defer func() { <-sem }()
				results <- fileResult{i, extractTrigramsFromFile(filepath.Join(b.dir, fi.path))}
			}(i, fi)
		}
		wg.Wait()
		close(results)
	}()

	posting := make(map[uint32][]postingEntry, 1<<17)
	for r := range results {
		for t, bl := range r.trigrams {
			posting[t] = append(posting[t], postingEntry{uint32(r.fileID), bl})
		}
	}

	// Posting lists must be sorted by fileID for binary-search intersect.
	for t := range posting {
		list := posting[t]
		sort.Slice(list, func(i, j int) bool { return list[i].fileID < list[j].fileID })
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

	// Invert old posting lists into per-file (trigram, bloom) pairs.
	// perFileTrigrams[oldID] and perFileBlooms[oldID] are parallel slices.
	perFileTrigrams := make([][]uint32, len(idx.files))
	perFileBlooms := make([][]uint8, len(idx.files))
	for _, oldID := range unchangedOldID {
		if oldID >= 0 && perFileTrigrams[oldID] == nil {
			perFileTrigrams[oldID] = make([]uint32, 0, 128)
			perFileBlooms[oldID] = make([]uint8, 0, 128)
		}
	}

	// Pruned trigrams had no posting list stored; treat them as present in all
	// unchanged files with bloom=0xFF (all groups possible) so writeIndex
	// re-prunes them correctly and bloom filtering is not falsely applied.
	var prunedTrigrams []uint32
	hasBloom := idx.version >= 4 && len(idx.bloomData) > 0
	data := idx.data
	for ti, t := range idx.trigrams {
		cnt := idx.counts[ti]
		if cnt == 0 {
			prunedTrigrams = append(prunedTrigrams, t)
			continue
		}
		off := idx.offsets[ti]
		relBase := (off - idx.postingBase) / 4
		for i := uint32(0); i < cnt; i++ {
			fid := binary.LittleEndian.Uint32(data[off+i*4:])
			if perFileTrigrams[fid] == nil {
				continue
			}
			perFileTrigrams[fid] = append(perFileTrigrams[fid], t)
			var bl uint8
			if hasBloom {
				bl = idx.bloomData[relBase+i]
			} else {
				bl = 0xFF // v3 index: no bloom data, conservative
			}
			perFileBlooms[fid] = append(perFileBlooms[fid], bl)
		}
	}
	for oldID, pft := range perFileTrigrams {
		if pft != nil {
			n := len(prunedTrigrams)
			perFileTrigrams[oldID] = append(pft, prunedTrigrams...)
			extra := make([]uint8, n)
			for i := range extra {
				extra[i] = 0xFF
			}
			perFileBlooms[oldID] = append(perFileBlooms[oldID], extra...)
		}
	}

	posting := make(map[uint32][]postingEntry)
	for newFID, fi := range newFiles {
		oldID := unchangedOldID[newFID]
		if oldID >= 0 {
			ts := perFileTrigrams[oldID]
			bs := perFileBlooms[oldID]
			for j, t := range ts {
				posting[t] = append(posting[t], postingEntry{uint32(newFID), bs[j]})
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

	prunedCount := 0
	for _, t := range trigrams {
		if pruneThreshold > 0 && len(posting[t]) > pruneThreshold {
			prunedCount++
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
	write32(version) // v4
	write32(uint32(len(files)))
	write32(uint32(len(trigrams)))

	// File table (v3+: path + mtime + size)
	for _, fi := range files {
		write16(uint16(len(fi.path)))
		w.WriteString(fi.path)
		write64(fi.mtime)
		write64(fi.size)
	}

	// Compute postingStart (byte offset of PostingLists section)
	fileTableSize := 0
	for _, fi := range files {
		fileTableSize += 2 + len(fi.path) + 16
	}
	postingStart := uint32(16 + fileTableSize + len(trigrams)*12)

	// Trigram table — assign offsets and track total posting entries
	offset := postingStart
	totalEntries := uint32(0)
	for _, t := range trigrams {
		cnt := uint32(len(posting[t]))
		write32(t)
		if pruneThreshold > 0 && int(cnt) > pruneThreshold {
			write32(0)
			write32(0) // pruned sentinel
		} else {
			write32(offset)
			write32(cnt)
			offset += cnt * 4
			totalEntries += cnt
		}
	}

	// Posting lists (fileIDs, non-pruned only)
	for _, t := range trigrams {
		if pruneThreshold > 0 && len(posting[t]) > pruneThreshold {
			continue
		}
		for _, e := range posting[t] {
			write32(e.fileID)
		}
	}

	// Bloom section (1 byte per posting entry, parallel to posting lists)
	for _, t := range trigrams {
		if pruneThreshold > 0 && len(posting[t]) > pruneThreshold {
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
	bloomKB := totalEntries / 1024
	fmt.Fprintf(os.Stderr,
		"Index written in %v: %d files, %d trigrams (%d pruned, freq>%.0f%%), bloom +%dKB -> %s\n",
		elapsed, len(files), len(trigrams), prunedCount, maxFreq*100, bloomKB, output)
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
	postingBase uint32 // byte offset where PostingLists section starts
	bloomData   []byte // parallel to posting entries; nil for v3 indexes
}

// loadIndex reads the index file. If useMmap is true, the file is memory-mapped
// (lower cold-start cost; data is not copied). Otherwise os.ReadFile is used.
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
		sz := int(fi.Size())
		if sz == 0 {
			f.Close()
			return nil, fmt.Errorf("index file is empty")
		}
		mapped, err := syscall.Mmap(int(f.Fd()), 0, sz, syscall.PROT_READ, syscall.MAP_SHARED)
		f.Close() // fd can be closed after mmap; mapping stays valid
		if err != nil {
			return nil, fmt.Errorf("mmap: %w", err)
		}
		data = mapped
		// Note: we intentionally don't Munmap — for a CLI tool the OS reclaims on exit.
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
	totalEntries := uint32(0)
	for i := 0; i < numTrigrams; i++ {
		if pos+12 > len(data) {
			return nil, fmt.Errorf("index corrupt at trigram table")
		}
		trigrams[i] = binary.LittleEndian.Uint32(data[pos:])
		offsets[i] = binary.LittleEndian.Uint32(data[pos+4:])
		counts[i] = binary.LittleEndian.Uint32(data[pos+8:])
		totalEntries += counts[i]
		pos += 12
	}

	postingBase := uint32(pos)

	var bloomData []byte
	if ver >= 4 {
		bloomStart := int(postingBase) + int(totalEntries)*4
		bloomEnd := bloomStart + int(totalEntries)
		if bloomEnd <= len(data) {
			bloomData = data[bloomStart:bloomEnd]
		}
	}

	return &index{
		version:     ver,
		files:       files,
		trigrams:    trigrams,
		offsets:     offsets,
		counts:      counts,
		data:        data,
		postingBase: postingBase,
		bloomData:   bloomData,
	}, nil
}

// findTrigram returns the table index for trigram t, or -1 if not found.
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

// postingList returns a copy of file IDs for trigram at table index ti.
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

// lookup returns (posting list, pruned) for trigram value t.
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

// bloomFilter refines candidates using the bloom section (v4 only).
// For each position i in phrase where phrase[i+3] exists, it checks that
// the bloom byte for (trigram phrase[i..i+2], fileID) has the bit set for
// the character group of phrase[i+3]. Files missing that bit are eliminated
// without being read from disk.
func (idx *index) bloomFilter(candidates []uint32, phrase []byte) []uint32 {
	if len(idx.bloomData) == 0 || len(phrase) < 4 {
		return candidates
	}
	for i := 0; i+4 <= len(phrase); i++ {
		t := uint32(phrase[i])<<16 | uint32(phrase[i+1])<<8 | uint32(phrase[i+2])
		nextBit := uint8(1 << (phrase[i+3] >> 5))

		ti := idx.findTrigram(t)
		if ti < 0 || idx.counts[ti] == 0 {
			continue // trigram absent or pruned — can't filter on it
		}

		off := idx.offsets[ti]
		cnt := idx.counts[ti]
		relBase := (off - idx.postingBase) / 4

		// Merge-scan candidates (sorted) against posting list (sorted by fileID).
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
			default: // fid > c: c not in posting list — keep conservatively
				filtered = append(filtered, c)
				ci++
			}
		}
		filtered = append(filtered, candidates[ci:]...) // rest past posting list end
		candidates = filtered
		if len(candidates) == 0 {
			break
		}
	}
	return candidates
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

func union(a, b []uint32) []uint32 {
	result := make([]uint32, 0, len(a)+len(b))
	i, j := 0, 0
	for i < len(a) && j < len(b) {
		switch {
		case a[i] < b[j]:
			result = append(result, a[i]); i++
		case a[i] == b[j]:
			result = append(result, a[i]); i++; j++
		default:
			result = append(result, b[j]); j++
		}
	}
	result = append(result, a[i:]...)
	result = append(result, b[j:]...)
	return result
}

func allFileIDs(idx *index) []uint32 {
	c := make([]uint32, len(idx.files))
	for i := range c {
		c[i] = uint32(i)
	}
	return c
}

// ---- search ----------------------------------------------------------------

type searchCmd struct {
	indexPath string
	pattern   string
	literal   bool // -F: treat pattern as fixed string, not regex
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

	// Build AND/OR trigram plan from the regexp AST.
	// This correctly handles alternations: (void|bool).*run\( becomes
	// And[Or[lits("void"),lits("bool")], lits("run(")] instead of
	// incorrectly ANDing all three.
	syntaxRe, _ := syntax.Parse(s.pattern, syntax.Perl)
	plan := regexpToPlan(syntaxRe)
	candidates, unconstrained := evalPlan(idx, plan)
	if unconstrained {
		candidates = allFileIDs(idx)
	}
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
					lo := lineNo - s.context
					if lo < 0 {
						lo = 0
					}
					hi := lineNo + s.context + 1
					if hi > len(lines) {
						hi = len(lines)
					}
					for i := lo; i < hi; i++ {
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

// runLiteral handles -F (fixed string) search.
// Three-phase: trigram intersection → bloom filter → bytes.Contains.
func (s *searchCmd) runLiteral(idx *index) error {
	needle := []byte(s.pattern)

	// Phase 1: trigram intersection using all trigrams of the full phrase.
	candidates := candidatesFromPhrase(idx, s.pattern)

	// Phase 2: bloom filter — eliminate files where the trigram→next-char
	// association makes the phrase impossible, without reading files.
	before := len(candidates)
	candidates = idx.bloomFilter(candidates, needle)
	fmt.Fprintf(os.Stderr, "Candidates: %d / %d files (bloom: %d→%d)\n",
		len(candidates), len(idx.files), before, len(candidates))

	rootDir := s.rootDir
	if rootDir == "" {
		rootDir = "."
	}

	// Phase 3: verify with bytes.Contains.
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
					lo := lineNo - s.context
					if lo < 0 {
						lo = 0
					}
					hi := lineNo + s.context + 1
					if hi > len(lines) {
						hi = len(lines)
					}
					for i := lo; i < hi; i++ {
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

// candidatesFromPhrase intersects posting lists for all trigrams in the literal phrase.
// Used only by the -F path; regex path uses evalPlan instead.
func candidatesFromPhrase(idx *index, phrase string) []uint32 {
	ts := extractTrigrams(phrase)
	cands, unconstrained := evalPlan(idx, planLits{ts})
	if unconstrained {
		return allFileIDs(idx)
	}
	return cands
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
	searchPattern := searchFlags.String("pattern", "", "regex pattern (or literal string with -F)")
	searchLiteral := searchFlags.Bool("literal", false, "treat pattern as fixed string, not regex")
	searchFlags.BoolVar(searchLiteral, "F", false, "shorthand for --literal")
	searchFilesOnly := searchFlags.Bool("files-only", false, "only print matching file paths")
	searchContext := searchFlags.Int("context", 2, "lines of context around matches")
	searchRoot := searchFlags.String("root", ".", "root directory for resolving file paths")
	searchMmap := searchFlags.Bool("mmap", false, "use mmap to load index (lower cold-start cost, experimental)")

	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "Usage: trigrep <build|update|search> [options]")
		os.Exit(1)
	}

	switch os.Args[1] {
	case "build":
		buildFlags.Parse(os.Args[2:])
		if err := (&buildCmd{
			dir:     *buildDir,
			output:  *buildOutput,
			exts:    *buildExts,
			ignore:  *buildIgnore,
			maxFreq: *buildMaxFreq,
		}).run(); err != nil {
			fmt.Fprintln(os.Stderr, "Error:", err)
			os.Exit(1)
		}
	case "update":
		updateFlags.Parse(os.Args[2:])
		if err := (&updateCmd{
			indexPath: *updateIndex,
			dir:       *updateDir,
			exts:      *updateExts,
			ignore:    *updateIgnore,
			maxFreq:   *updateMaxFreq,
		}).run(); err != nil {
			fmt.Fprintln(os.Stderr, "Error:", err)
			os.Exit(1)
		}
	case "search":
		searchFlags.Parse(os.Args[2:])
		if *searchPattern == "" {
			fmt.Fprintln(os.Stderr, "Error: --pattern required")
			os.Exit(1)
		}
		if err := (&searchCmd{
			indexPath: *searchIndex,
			pattern:   *searchPattern,
			literal:   *searchLiteral,
			filesOnly: *searchFilesOnly,
			context:   *searchContext,
			rootDir:   *searchRoot,
			useMmap:   *searchMmap,
		}).run(); err != nil {
			fmt.Fprintln(os.Stderr, "Error:", err)
			os.Exit(1)
		}
	default:
		fmt.Fprintln(os.Stderr, "Unknown command:", os.Args[1])
		os.Exit(1)
	}
}
