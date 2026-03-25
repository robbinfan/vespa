package main

import (
	"bufio"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
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
// count == 0 in TrigramTable means the trigram was pruned (too frequent).

const magic = 0x54524749 // "TRGI"
const version = 3

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

// extractTrigramsFromFile reads a file and returns the set of unique trigrams.
func extractTrigramsFromFile(absPath string) map[uint32]bool {
	f, err := os.Open(absPath)
	if err != nil {
		return nil
	}
	defer f.Close()
	content, err := io.ReadAll(io.LimitReader(f, 1<<20))
	if err != nil {
		return nil
	}
	seen := make(map[uint32]bool)
	for i := 0; i+3 <= len(content); i++ {
		t := uint32(content[i])<<16 | uint32(content[i+1])<<8 | uint32(content[i+2])
		seen[t] = true
	}
	return seen
}

// ---- build (full) ----------------------------------------------------------

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
	fmt.Fprintf(os.Stderr, "Indexing %d files...\n", len(files))

	posting := make(map[uint32][]uint32)
	for i, fi := range files {
		if i%1000 == 0 && i > 0 {
			fmt.Fprintf(os.Stderr, "  %d/%d files processed\n", i, len(files))
		}
		trigrams := extractTrigramsFromFile(filepath.Join(b.dir, fi.path))
		for t := range trigrams {
			posting[t] = append(posting[t], uint32(i))
		}
	}

	if err := writeIndex(b.output, files, posting, b.maxFreq, start); err != nil {
		return err
	}
	return nil
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

	// Load existing index.
	idx, err := loadIndex(u.indexPath)
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

	// Collect current files from disk.
	newFiles, err := collectFiles(u.dir, u.exts, u.ignore)
	if err != nil {
		return err
	}

	// Build lookup: rel-path → old index position + metadata.
	type oldMeta struct {
		idx   int
		mtime int64
		size  int64
	}
	oldByPath := make(map[string]oldMeta, len(idx.files))
	for i, fi := range idx.files {
		oldByPath[fi.path] = oldMeta{i, fi.mtime, fi.size}
	}

	// Classify files.
	unchangedOldID := make([]int, len(newFiles)) // newFID → oldFID (-1 if changed/new)
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

	// If most files changed, a full rebuild is cheaper than inversion.
	if unchanged < len(newFiles)/2 {
		fmt.Fprintf(os.Stderr, "Too many changes, doing full rebuild.\n")
		b := &buildCmd{dir: u.dir, output: u.indexPath, exts: u.exts, ignore: u.ignore, maxFreq: u.maxFreq}
		return b.run()
	}

	// Invert the old posting lists into per-oldFile trigram slices.
	// Use [][]uint32 (not maps) to avoid hash overhead on 13M+ pairs.
	// Mark slots for changed/deleted files as nil to skip them.
	perFileTrigrams := make([][]uint32, len(idx.files))
	for newFID, oldID := range unchangedOldID {
		if oldID >= 0 {
			_ = newFID
			if perFileTrigrams[oldID] == nil {
				perFileTrigrams[oldID] = make([]uint32, 0, 128)
			}
		}
	}
	// Collect pruned trigrams: their posting lists weren't stored, so we can't
	// invert them normally. Treat them as present in ALL unchanged files so
	// writeIndex sees the correct high count and re-prunes them. Without this,
	// a pruned common trigram appears in only the changed files, causing
	// false-negative search results.
	var prunedTrigrams []uint32
	data := idx.data
	for ti, t := range idx.trigrams {
		cnt := idx.counts[ti]
		if cnt == 0 {
			prunedTrigrams = append(prunedTrigrams, t)
			continue
		}
		off := idx.offsets[ti]
		for i := uint32(0); i < cnt; i++ {
			fid := binary.LittleEndian.Uint32(data[off+i*4:])
			if perFileTrigrams[fid] != nil {
				perFileTrigrams[fid] = append(perFileTrigrams[fid], t)
			}
		}
	}
	// Add all pruned trigrams to every unchanged file.
	for oldID, pft := range perFileTrigrams {
		if pft != nil {
			perFileTrigrams[oldID] = append(pft, prunedTrigrams...)
		}
	}

	// Build new posting map: reuse inverted slices for unchanged files,
	// re-read disk only for changed/new files.
	posting := make(map[uint32][]uint32)
	for newFID, fi := range newFiles {
		oldID := unchangedOldID[newFID]
		if oldID >= 0 {
			for _, t := range perFileTrigrams[oldID] {
				posting[t] = append(posting[t], uint32(newFID))
			}
		} else {
			for t := range extractTrigramsFromFile(filepath.Join(u.dir, fi.path)) {
				posting[t] = append(posting[t], uint32(newFID))
			}
		}
	}

	return writeIndex(u.indexPath, newFiles, posting, u.maxFreq, start)
}

// ---- index write -----------------------------------------------------------

func writeIndex(output string, files []fileInfo, posting map[uint32][]uint32, maxFreq float64, start time.Time) error {
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
	if pruneThreshold > 0 {
		for _, t := range trigrams {
			if len(posting[t]) > pruneThreshold {
				prunedCount++
			}
		}
	}

	if err := os.MkdirAll(filepath.Dir(output), 0755); err != nil {
		return err
	}
	// Write to temp file, then rename for atomicity.
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
	write32(version) // v3
	write32(uint32(len(files)))
	write32(uint32(len(trigrams)))

	// File table (v3: path + mtime + size)
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
		fileTableSize += 2 + len(fi.path) + 16 // pathLen + path + mtime(8) + size(8)
	}
	trigramTableSize := len(trigrams) * 12
	postingStart := uint32(headerSize + fileTableSize + trigramTableSize)

	// Trigram table
	offset := postingStart
	for _, t := range trigrams {
		cnt := uint32(len(posting[t]))
		write32(t)
		if pruneThreshold > 0 && int(cnt) > pruneThreshold {
			write32(0) // offset sentinel
			write32(0) // count=0 signals pruned
		} else {
			write32(offset)
			write32(cnt)
			offset += cnt * 4
		}
	}

	// Posting lists (non-pruned only)
	for _, t := range trigrams {
		if pruneThreshold > 0 && len(posting[t]) > pruneThreshold {
			continue
		}
		for _, fid := range posting[t] {
			write32(fid)
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
			elapsed, len(files), len(trigrams), prunedCount, maxFreq*100, output)
	} else {
		fmt.Fprintf(os.Stderr,
			"Index written in %v: %d files, %d unique trigrams -> %s\n",
			elapsed, len(files), len(trigrams), output)
	}
	return nil
}

// ---- index load ------------------------------------------------------------

type index struct {
	version  uint32
	files    []fileInfo
	trigrams []uint32 // sorted
	offsets  []uint32
	counts   []uint32 // count==0 means pruned
	data     []byte
}

func loadIndex(path string) (*index, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
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

	return &index{
		version:  ver,
		files:    files,
		trigrams: trigrams,
		offsets:  offsets,
		counts:   counts,
		data:     data,
	}, nil
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
		return nil, false
	}
	if idx.counts[lo] == 0 {
		return nil, true // pruned
	}
	return idx.postingList(lo), false
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

// ---- search ----------------------------------------------------------------

type searchCmd struct {
	indexPath string
	pattern   string
	filesOnly bool
	context   int
	rootDir   string
}

func (s *searchCmd) run() error {
	idx, err := loadIndex(s.indexPath)
	if err != nil {
		return fmt.Errorf("loading index: %w", err)
	}

	re, err := regexp.Compile(s.pattern)
	if err != nil {
		return fmt.Errorf("invalid pattern: %w", err)
	}

	literals := extractLiterals(s.pattern)

	var candidates []uint32
	if len(literals) == 0 {
		candidates = make([]uint32, len(idx.files))
		for i := range candidates {
			candidates[i] = uint32(i)
		}
	} else {
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
			candidates = make([]uint32, len(idx.files))
			for i := range candidates {
				candidates[i] = uint32(i)
			}
		}
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
	updateMaxFreq := updateFlags.Float64("max-freq", 0.5, "prune threshold (inherits from existing index if not set)")

	searchFlags := flag.NewFlagSet("search", flag.ExitOnError)
	searchIndex := searchFlags.String("index", ".claude/trigram-index.bin", "index file")
	searchPattern := searchFlags.String("pattern", "", "regex pattern")
	searchFilesOnly := searchFlags.Bool("files-only", false, "only print matching file paths")
	searchContext := searchFlags.Int("context", 2, "lines of context")
	searchRoot := searchFlags.String("root", ".", "root directory for resolving file paths")

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
			filesOnly: *searchFilesOnly,
			context:   *searchContext,
			rootDir:   *searchRoot,
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
