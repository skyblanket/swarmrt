// swarmrt-feedback — shared feedback aggregator for SwarmRT MCP.
//
// Single-binary Go HTTP server. SQLite on a Fly volume. Public read,
// anonymous write, per-IP rate-limited. No auth, no PII, no file
// contents — just structured feedback from agents using swarmrt tools.
//
// The point is a Maury-style abstract log: thousands of captains
// contribute observations, everyone gets better charts.
package main

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	_ "modernc.org/sqlite"
)

const (
	serviceName     = "swarmrt-feedback"
	serviceVersion  = "0.1.0"
	maxMsgLen       = 4000
	maxFieldLen     = 256
	rateLimitPerHr  = 60
	defaultPageSize = 50
	maxPageSize     = 500
)

var db *sql.DB

// --- rate limiter ---------------------------------------------------------

type rateLimiter struct {
	mu     sync.Mutex
	window map[string][]int64 // ip -> list of unix timestamps within last hour
}

func newRateLimiter() *rateLimiter {
	return &rateLimiter{window: map[string][]int64{}}
}

func (r *rateLimiter) allow(ip string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	now := time.Now().Unix()
	cutoff := now - 3600
	times := r.window[ip]
	fresh := times[:0]
	for _, t := range times {
		if t > cutoff {
			fresh = append(fresh, t)
		}
	}
	if len(fresh) >= rateLimitPerHr {
		r.window[ip] = fresh
		return false
	}
	fresh = append(fresh, now)
	r.window[ip] = fresh
	return true
}

var rl = newRateLimiter()

// --- schema ---------------------------------------------------------------

const schemaSQL = `
CREATE TABLE IF NOT EXISTS feedback (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    received_at   INTEGER NOT NULL,
    machine_id    TEXT,
    session_id    TEXT,
    swarmrt_ver   TEXT NOT NULL,
    model         TEXT,
    category      TEXT NOT NULL CHECK (category IN ('bug','confusion','wish','works-well')),
    tool          TEXT,
    severity      TEXT NOT NULL CHECK (severity IN ('low','med','high')),
    message       TEXT NOT NULL,
    suggested_fix TEXT,
    context       TEXT
);
CREATE INDEX IF NOT EXISTS idx_feedback_received ON feedback(received_at);
CREATE INDEX IF NOT EXISTS idx_feedback_cat_sev ON feedback(category, severity);
CREATE UNIQUE INDEX IF NOT EXISTS uniq_session_msg
    ON feedback(COALESCE(machine_id,''), COALESCE(session_id,''), message);
`

func initSchema(db *sql.DB) error {
	_, err := db.Exec(schemaSQL)
	return err
}

// --- helpers --------------------------------------------------------------

func writeJSON(w http.ResponseWriter, code int, v interface{}) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}

func clientIP(r *http.Request) string {
	// Fly.io populates Fly-Client-IP with the real client address.
	if v := r.Header.Get("Fly-Client-IP"); v != "" {
		return v
	}
	if v := r.Header.Get("X-Forwarded-For"); v != "" {
		if i := strings.Index(v, ","); i >= 0 {
			return strings.TrimSpace(v[:i])
		}
		return strings.TrimSpace(v)
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func nullable(s string) interface{} {
	if s == "" {
		return nil
	}
	return s
}

// Refuses filesystem paths in free-text fields so agents can't accidentally
// exfiltrate a user's directory structure through a feedback report.
func containsPathLike(s string) bool {
	if s == "" {
		return false
	}
	if strings.Contains(s, "/Users/") ||
		strings.Contains(s, "/home/") ||
		strings.Contains(s, "/root/") ||
		strings.Contains(s, "/var/") ||
		strings.Contains(s, "/tmp/") ||
		strings.Contains(s, "/private/") ||
		strings.Contains(s, "C:\\") ||
		strings.Contains(s, "C:/") {
		return true
	}
	return false
}

func validCategory(c string) bool {
	switch c {
	case "bug", "confusion", "wish", "works-well":
		return true
	}
	return false
}

func validSeverity(s string) bool {
	switch s {
	case "low", "med", "high":
		return true
	}
	return false
}

// --- middleware -----------------------------------------------------------

func middleware(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		start := time.Now()
		h.ServeHTTP(w, r)
		log.Printf("%s %s %s %s", clientIP(r), r.Method, r.URL.Path, time.Since(start))
	})
}

// --- handlers -------------------------------------------------------------

func rootHandler(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"service": serviceName,
		"version": serviceVersion,
		"endpoints": []string{
			"POST /v1/report",
			"GET  /v1/feedback",
			"GET  /v1/stats",
			"GET  /v1/healthz",
		},
		"source":       "https://github.com/skyblanket/swarmrt",
		"docs":         "https://github.com/skyblanket/swarmrt/blob/main/mcp/README.md",
		"privacy_note": "Anonymous writes, public reads. Rejects filesystem paths in free-text to prevent accidental exfiltration.",
	})
}

func healthzHandler(w http.ResponseWriter, r *http.Request) {
	if err := db.Ping(); err != nil {
		writeErr(w, http.StatusServiceUnavailable, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

type reportBody struct {
	SwarmrtVer   string `json:"swarmrt_ver"`
	MachineID    string `json:"machine_id"`
	SessionID    string `json:"session_id"`
	Model        string `json:"model"`
	Category     string `json:"category"`
	Tool         string `json:"tool"`
	Severity     string `json:"severity"`
	Message      string `json:"message"`
	SuggestedFix string `json:"suggested_fix"`
	Context      string `json:"context"`
}

func postReportHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "POST only")
		return
	}

	ip := clientIP(r)
	if !rl.allow(ip) {
		writeErr(w, http.StatusTooManyRequests,
			fmt.Sprintf("rate limit: %d reports per IP per hour", rateLimitPerHr))
		return
	}

	// Cap request body size to prevent DoS via huge payloads.
	r.Body = http.MaxBytesReader(w, r.Body, 64*1024)
	var body reportBody
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(&body); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}

	// Required fields
	if body.SwarmrtVer == "" {
		writeErr(w, http.StatusBadRequest, "swarmrt_ver required")
		return
	}
	if body.Message == "" {
		writeErr(w, http.StatusBadRequest, "message required")
		return
	}
	if !validCategory(body.Category) {
		writeErr(w, http.StatusBadRequest, "category must be one of: bug, confusion, wish, works-well")
		return
	}
	if !validSeverity(body.Severity) {
		writeErr(w, http.StatusBadRequest, "severity must be one of: low, med, high")
		return
	}

	// Length caps
	if len(body.Message) > maxMsgLen {
		writeErr(w, http.StatusBadRequest, fmt.Sprintf("message too long (max %d)", maxMsgLen))
		return
	}
	// Short-field caps (identifiers only)
	for name, v := range map[string]string{
		"swarmrt_ver": body.SwarmrtVer,
		"model":       body.Model,
		"tool":        body.Tool,
		"machine_id":  body.MachineID,
		"session_id":  body.SessionID,
	} {
		if len(v) > maxFieldLen {
			writeErr(w, http.StatusBadRequest, fmt.Sprintf("%s too long (max %d)", name, maxFieldLen))
			return
		}
	}
	// Long-field caps (free text — same budget as message)
	for name, v := range map[string]string{
		"suggested_fix": body.SuggestedFix,
		"context":       body.Context,
	} {
		if len(v) > maxMsgLen {
			writeErr(w, http.StatusBadRequest, fmt.Sprintf("%s too long (max %d)", name, maxMsgLen))
			return
		}
	}

	// Privacy guard: reject filesystem paths in any free-text field.
	for _, s := range []string{body.Message, body.Context, body.SuggestedFix} {
		if containsPathLike(s) {
			writeErr(w, http.StatusBadRequest,
				"free-text fields must not contain filesystem paths (privacy). Summarize instead.")
			return
		}
	}

	now := time.Now().Unix()
	res, err := db.Exec(`
        INSERT OR IGNORE INTO feedback
            (received_at, machine_id, session_id, swarmrt_ver, model, category, tool, severity, message, suggested_fix, context)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		now,
		nullable(body.MachineID),
		nullable(body.SessionID),
		body.SwarmrtVer,
		nullable(body.Model),
		body.Category,
		nullable(body.Tool),
		body.Severity,
		body.Message,
		nullable(body.SuggestedFix),
		nullable(body.Context),
	)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "db insert: "+err.Error())
		return
	}
	id, _ := res.LastInsertId()
	rowsAffected, _ := res.RowsAffected()
	if rowsAffected == 0 {
		writeJSON(w, http.StatusOK, map[string]interface{}{
			"deduped":     true,
			"received_at": now,
			"note":        "identical (machine_id, session_id, message) already present",
		})
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"id":          id,
		"received_at": now,
	})
}

func getFeedbackHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "GET only")
		return
	}
	q := r.URL.Query()

	limit := defaultPageSize
	if l := q.Get("limit"); l != "" {
		if n, err := strconv.Atoi(l); err == nil && n > 0 {
			if n > maxPageSize {
				n = maxPageSize
			}
			limit = n
		}
	}

	var since int64
	if s := q.Get("since"); s != "" {
		since, _ = strconv.ParseInt(s, 10, 64)
	}

	where := []string{"received_at >= ?"}
	args := []interface{}{since}

	if c := q.Get("category"); c != "" {
		if !validCategory(c) {
			writeErr(w, http.StatusBadRequest, "invalid category")
			return
		}
		where = append(where, "category = ?")
		args = append(args, c)
	}
	if t := q.Get("tool"); t != "" {
		where = append(where, "tool = ?")
		args = append(args, t)
	}
	switch q.Get("severity_min") {
	case "med":
		where = append(where, "severity IN ('med','high')")
	case "high":
		where = append(where, "severity = 'high'")
	}

	query := `SELECT id, received_at, swarmrt_ver, model, category, tool, severity, message, suggested_fix, context
              FROM feedback
              WHERE ` + strings.Join(where, " AND ") + `
              ORDER BY received_at DESC
              LIMIT ?`
	args = append(args, limit)

	rows, err := db.Query(query, args...)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "db query: "+err.Error())
		return
	}
	defer rows.Close()

	items := make([]map[string]interface{}, 0, limit)
	for rows.Next() {
		var id, recv int64
		var ver, cat, sev, msg string
		var model, tool, fix, ctx sql.NullString
		if err := rows.Scan(&id, &recv, &ver, &model, &cat, &tool, &sev, &msg, &fix, &ctx); err != nil {
			continue
		}
		item := map[string]interface{}{
			"id":          id,
			"received_at": recv,
			"swarmrt_ver": ver,
			"category":    cat,
			"severity":    sev,
			"message":     msg,
		}
		if model.Valid {
			item["model"] = model.String
		}
		if tool.Valid {
			item["tool"] = tool.String
		}
		if fix.Valid {
			item["suggested_fix"] = fix.String
		}
		if ctx.Valid {
			item["context"] = ctx.String
		}
		items = append(items, item)
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"items": items,
		"count": len(items),
	})
}

func getStatsHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "GET only")
		return
	}

	var since int64
	if s := r.URL.Query().Get("since"); s != "" {
		since, _ = strconv.ParseInt(s, 10, 64)
	}

	counts := map[string]int{"bug": 0, "confusion": 0, "wish": 0, "works-well": 0}
	rows, err := db.Query(`SELECT category, COUNT(*) FROM feedback WHERE received_at >= ? GROUP BY category`, since)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	for rows.Next() {
		var cat string
		var n int
		if err := rows.Scan(&cat, &n); err == nil {
			counts[cat] = n
		}
	}
	rows.Close()

	var total int
	_ = db.QueryRow(`SELECT COUNT(*) FROM feedback WHERE received_at >= ?`, since).Scan(&total)

	topTools := []map[string]interface{}{}
	rows2, err := db.Query(`SELECT tool, COUNT(*) FROM feedback WHERE received_at >= ? AND tool IS NOT NULL GROUP BY tool ORDER BY 2 DESC LIMIT 10`, since)
	if err == nil {
		for rows2.Next() {
			var tool string
			var n int
			if err := rows2.Scan(&tool, &n); err == nil {
				topTools = append(topTools, map[string]interface{}{"name": tool, "count": n})
			}
		}
		rows2.Close()
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"since":     since,
		"total":     total,
		"counts":    counts,
		"top_tools": topTools,
	})
}

// --- main -----------------------------------------------------------------

func main() {
	dbPath := os.Getenv("SWARMRT_DB_PATH")
	if dbPath == "" {
		dbPath = "/data/swarmrt.db"
	}

	// modernc.org/sqlite accepts _pragma=... via DSN for WAL + busy timeout.
	dsn := dbPath + "?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)&_pragma=foreign_keys(1)"
	var err error
	db, err = sql.Open("sqlite", dsn)
	if err != nil {
		log.Fatalf("sql.Open: %v", err)
	}
	db.SetMaxOpenConns(1) // SQLite — serialize writes cleanly
	defer db.Close()

	if err := initSchema(db); err != nil {
		log.Fatalf("initSchema: %v", err)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", rootHandler)
	mux.HandleFunc("/v1/healthz", healthzHandler)
	mux.HandleFunc("/v1/report", postReportHandler)
	mux.HandleFunc("/v1/feedback", getFeedbackHandler)
	mux.HandleFunc("/v1/stats", getStatsHandler)

	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}

	srv := &http.Server{
		Addr:              ":" + port,
		Handler:           middleware(mux),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      15 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	// Graceful shutdown so the SQLite WAL gets flushed on SIGTERM.
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)

	go func() {
		log.Printf("%s v%s listening on %s (db=%s)", serviceName, serviceVersion, srv.Addr, dbPath)
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatalf("ListenAndServe: %v", err)
		}
	}()

	<-sigs
	log.Printf("shutting down...")
	_ = srv.Close()
	_ = db.Close()
}
