;;; emacs-webview2.el --- webview2 in Emacs -*- lexical-binding: t; -*-

;; Copyright (C) 2026 include-yy <yy@egh0bww1.com>

;; Author: include-yy <yy@egh0bww1.com>
;; Maintainer: include-yy <yy@egh0bww1.com>
;; Created: 2026-02-17 21:53+0800

;; Package-Version: 0.1
;; Package-Requires: ((emacs "31"))
;; Keywords: tools, html
;; URL: https://github.com/include-yy/emacs-webview2

;; SPDX-License-Identifier: GPL-3.0-or-later

;; emacs-webview2 is free software; you can redistribute it and/or
;; modify it under the terms of the GNU General Public License as
;; published by the Free Software Foundation, either version 3 of the
;; License, or (at your option) any later version.

;; emacs-webview2 is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with emacs-webview2.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;;; Code:
(unless (and (eq system-type 'windows-nt)
             (>= (car (w32-version)) 10))
  (error "Please use this package on Windows 10 or above"))

(require 'jsonrpc)
(require 'cl-lib)
(require 'map)
(require 'timeout)

(defgroup emacs-webview2 nil
  "Options for emacs webview2 binding."
  :tag "Emacs Webview2"
  :group 'applications)

(defcustom t-env-alist
  '(("default" . ( :language "zh-CN"
                   :additional_browser_arguments nil
                   :user_data_dir nil)))
  "Docstring"
  :type '(alist :key-type string :value-type plist)
  :group 'emacs-webview2)

(defcustom t-default-env "default"
  "The default webview2 environment."
  :type 'string
  :group 'emacs-webview2)

(defcustom t-default-intercept-keys
  '("C-g" "M-x" "C-x" "M-:" "C-c" "C-[")
  "Webview2 intercept keys"
  :type '(repeat string)
  :group 'emacs-webview2)

(defconst t--dir
  (if (not load-in-progress) default-directory
    (file-name-directory load-file-name))
  "Package's root directory.")

(cl-defstruct (t--manager (:constructor t--manager-make)
                          (:copier nil))
  "Manager for WebView2 instances and resources."
  (conn
   nil :type jsonrpc-process-connection
   :documentation "JSONRPC connection object.")
  (dying
   nil :type boolean
   :documentation "Non-nil if the connection is shutting down.")
  (wv-map
   (make-hash-table :test #'eq) :type hash-table
   :documentation "Mapping of IDs to `emacs-webview2--webview' structures.")
  (buf-map
   (make-hash-table :test #'eq) :type hash-table
   :documentation "Mapping of IDs to bound Emacs buffers.")
  (envs
   (make-hash-table :test #'equal) :type hash-table
   :documentation "Initialized WebView2 environments."))

(cl-defstruct (t--webview (:constructor t--webview-make)
                          (:copier nil))
  "WebView2 instance wrapper."
  (id             0   :type integer    :documentation "WebView2 instance id.")
  (buffer         nil :type buffer     :documentation "Buffer attached to.")
  (frame          nil :type frame      :documentation "Parent frame.")
  (last-bounds    nil :type vector     :documentation "Last rectangle bound.")
  (last-visible   0   :type integer    :documentation "Last visible state.")
  (rect-fn        nil :type function   :documentation "Bound calc function.")
  (env            nil :type string     :documentation "Instance's environment")
  (intercept-keys nil :type hash-table :documentation "Intercept Keys."))

(defvar t--mgr (t--manager-make)
  "Global webview2 manager.")

(defvar-local t-wv nil
  "Buffer-local Webview2 structure.")

(defun t--get-frame-hwnd (&optional frame)
  "Get frame's Win32 HWND."
  (let ((id (frame-parameter (or frame (selected-frame)) 'window-id)))
    (string-to-number id)))

(defun t--get-window-rect (&optional window)
  "Get WINDOW bound rect."
  (cl-coerce (window-body-pixel-edges window) 'vector))

(defun t--get-prioritized-window (wv buffer)
  (let* ((sel-win (selected-window))
         (sel-frame (window-frame sel-win))
         (last-frame (t--webview-frame wv))
         (has-focus (frame-focus-state sel-frame)))
    (cond ((and has-focus
                (eq (window-buffer sel-win) buffer))
           sel-win)
          ((and (frame-live-p last-frame)
                (get-buffer-window buffer last-frame)))
          (t (get-buffer-window buffer 'visible)))))

(defun t--alive-p ()
  "Check if the connection is alive."
  (let* ((conn (o-conn t--mgr))
         (dying (o-dying t--mgr)))
    (and (jsonrpc-process-connection-p conn)
         (not dying)
         (jsonrpc-running-p conn))))

(defun t--calculate-ui-diff (wv &optional no-modify)
  (let* ((buffer (t--webview-buffer wv))
         (window (and buffer (t--get-prioritized-window wv buffer)))
         (target-vis (if window 1 0))
         (rect-fn (or (t--webview-rect-fn wv) #'t--get-window-rect))
         (target-rect (when window (funcall rect-fn window)))
         (target-frame (when window (window-frame window)))
         (last-vis (t--webview-last-visible wv))
         (last-rect (t--webview-last-bounds wv))
         (last-frame (t--webview-frame wv))
         (diff-vis (unless (= target-vis last-vis) target-vis))
         (diff-rect (unless (equal target-rect last-rect) target-rect))
         (diff-parent (unless (eq target-frame last-frame)
                        (if (not target-frame) 0
                            (t--get-frame-hwnd target-frame)))))
    (when (or diff-vis diff-rect diff-parent)
      (unless no-modify
        (when diff-vis (setf (t--webview-last-visible wv) target-vis))
        (when diff-rect (setf (t--webview-last-bounds wv) target-rect))
        (when diff-parent (setf (t--webview-frame wv) target-frame)))
      (vector (t--webview-id wv) diff-vis diff-rect diff-parent))))

(defun o-sync-ui (wv)
  (when-let* ((res (t--calculate-ui-diff wv)))
    (m-wv/sync-ui-batch (vector res))))

(defun o-get-buffer (id)
  (when-let* ((wv (gethash id (o-wv-map t--mgr))))
    (t--webview-buffer wv)))

(defun o-focus-by-id (id)
  "Set focus to id-coressponded buffer."
  (when-let* ((target-buf (o-get-buffer id)))
    (let ((target-win (get-buffer-window target-buf 'visible)))
      (if target-win (select-window target-win)
        (switch-to-buffer target-buf))
      (let ((frame (window-frame (selected-window))))
        (select-frame-set-input-focus frame)))))

(defun o-ensure-env (env-name)
  (let ((table (o-envs t--mgr)))
    (unless (gethash env-name table)
      (if-let* ((config (cdr (assoc env-name t-env-alist))))
          (progn
            (m-env/create (append (list :name env-name) config))
            (puthash env-name t table))
        (error "Undefined WebView2 Environment [%s]")))))

(defun o-register-env (name &rest plist)
  (let ((table (o-envs t--mgr)))
    (unless (gethash name table)
      (let ((rpc-params (append (list :name name) plist)))
        (m-env/create rpc-params)
        (puthash name t table)
        (message "WebView2 Environment [%s] Registered." name)))))

(defconst t--vkey-map
  '((?\[ . 219) (?\] . 221) (?\; . 186) (?= . 187)
    (?, . 188) (?- . 189) (?. . 190) (?/ . 191)
    (?` . 192) (?\\ . 220) (?' . 222)
    ('backspace 8) ('tab 9) ('return 13)
    ('escape 27) ('space 32)
    ('left  37) ('up    38) ('right 39) ('down  40)
    ('prior 33) ('next  34) ('end   35) ('home  36)
    ('delete 46) ('insert 45)
    ('f1 112) ('f2 113) ('f3 114) ('f4 115)
    ('f5 116) ('f6 117) ('f7 118) ('f8 119)
    ('f9 120) ('f10 121) ('f11 122) ('f12 123)))

(defconst t--modifier-value-map
  `((super . ,(lsh 1 23))
    (shift . ,(lsh 1 25))
    (control . ,(lsh 1 26))
    (meta . ,(lsh 1 27))))

(defun t--get-modifiers (val)
  (let ((res))
    (unless (zerop (logand val (lsh 1 23)))
      (push 'super res))
    (unless (zerop (logand val (lsh 1 25)))
      (push 'shift res))
    (unless (zerop (logand val (lsh 1 26)))
      (push 'control res))
    (unless (zerop (logand val (lsh 1 27)))
      (push 'meta res))
    res))

(defun t--get-modifiers-value (ms)
  (cl-reduce
   (lambda (s a)
     (+ s (or (alist-get a t--modifier-value-map) 0)))
   ms :initial-value 0))

(defun t--encode-key-to-uint (key)
  (unless (and (stringp key) (not (string-empty-p key)))
    (error "Require non-empty string as Key"))
  (let ((vec (key-parse key)))
    (when-let* ((_ (>= (length vec) 1))
                (num (aref vec 0)))
      (let* ((ms (event-modifiers num))
             (k (event-basic-type num))
             (vk (cond
                  ((<= ?a k ?z) (upcase k))
                  ((alist-get k t--vkey-map))
                  (t k))))
        (+ (or vk 0) (t--get-modifiers-value ms))))))

(defun t--decode-uint-to-key (uint)
  (let* ((ms (t--get-modifiers uint))
         (ms-val (t--get-modifiers-value ms))
         (k (- uint ms-val))
         (base (cond
                ((<= ?A k ?Z) (downcase k))
                ((car (rassoc k t--vkey-map)))
                (t k))))
    (event-convert-list (append ms (list base)))))

(defun o-sync-intercept-keys (wv)
  (let* ((id (t--webview-id wv))
         (table (t--webview-intercept-keys wv))
         (uints (hash-table-keys table)))
    (m-wv/set-intercept-keys id (vconcat uints))))

(defun o-add-intercept-key (wv key-str)
  (let* ((uint (t--encode-key-to-uint key-str))
         (table (t--webview-intercept-keys wv)))
    (unless (gethash uint table)
      (puthash uint key-str table)
      (o-sync-intercept-keys wv))))

(defun o-remove-intercept-key (wv key-str)
  (let* ((uint (t--encode-key-to-uint key-str))
         (table (t--webview-intercept-keys wv)))
    (when (gethash uint table)
      (remhash uint table)
      (o-sync-intercept-keys wv))))

(defun o-clear-intercept-keys (wv)
  (let* ((table (t--webview-intercept-keys wv)))
    (clrhash table)
    (o-sync-intercept-keys wv)))

(defun o-register-wv (wv)
  (let* ((id (t--webview-id wv)))
    (puthash id wv (o-wv-map t--mgr))))

(defun t-on-kill-buffer ()
  (when (and (boundp 't-wv) t-wv (t--alive-p))
    (o-dispose t-wv)))

(defun o-attach (wv buffer)
  (let* ((id (t--webview-id wv)))
    (with-current-buffer buffer
      (setq-local t-wv id)
      (setf (t--webview-buffer wv) buffer)
      (add-hook 'kill-buffer-hook #'t-on-kill-buffer nil t))))

(defun o-detach (wv)
  (when-let* ((buf (t--webview-buffer wv)))
    (o-deactivate wv)
    (with-current-buffer buf
      (kill-local-variable 't-wv)
      (remove-hook 'kill-buffer-hook #'t-on-kill-buffer))
    (setf (t--webview-buffer wv) nil)))

(defun o-activate (wv-or-id)
  (when-let* ((wv (if (t--webview-p wv-or-id) wv-or-id
                    (gethash wv-or-id (o-wv-map t--mgr)))))
    (let* ((id (t--webview-id wv))
           (buf (t--webview-buffer wv))
           (mgr t--mgr)
           (map (o-buf-map mgr))
           (old-cnt (hash-table-count map)))
      (puthash id buf map)
      (when (and (zerop old-cnt)
                 (plusp (hash-table-count map)))
        (t--register-hooks))
      (when-let* ((win (get-buffer-window buf 'visible)))
        (o-sync-ui wv)))))

(defun o-deactivate (wv)
  (let* ((id (t--webview-id wv))
         (mgr t--mgr)
         (map (o-buf-map mgr)))
    (remhash id map)
    (o-sync-ui wv)
    (when (zerop (hash-table-count map))
      (t--unregister-hooks))))

(cl-defun o-spawn (&key buffer url env rect rect-fn
                        (activate t) (keys t-default-intercept-keys))
  (let* ((env (or env t-default-env))
         (_ (o-ensure-env env))
         (win (and buffer (get-buffer-window buffer 'visible)))
         (rect (or rect
                   (and rect-fn win (funcall rect-fn win))
                   (and win (t--get-window-rect win))))
         (frame (and win (window-frame win)))
         (hwnd (when frame (t--get-frame-hwnd frame)))
         (visible (and win t))
         (key-table (let ((tbl (make-hash-table :test #'eq)))
                      (mapc (lambda (key-str)
                              (let ((val (t--encode-key-to-uint key-str)))
                                (puthash val key-str tbl)))
                            keys)
                      tbl))
         (id (m-wv/create hwnd visible rect url env))
         (wv (t--webview-make
              :id id :buffer buffer :frame frame
              :last-bounds (or rect [0 0 0 0])
              :last-visible (if visible 1 0)
              :rect-fn rect-fn :env env
              :intercept-keys key-table)))
    (o-register-wv wv)
    (o-sync-intercept-keys wv)
    (when buffer
      (o-attach wv buffer)
      (when (and activate win)
        (o-activate wv)))
    wv))

(defun o-dispose (wv-or-id)
  (when-let* ((wv (if (t--webview-p wv-or-id) wv-or-id
                    (gethash wv-or-id (o-wv-map t--mgr))))
              (id (t--webview-id wv)))
    (o-detach wv)
    (remhash id (o-wv-map t--mgr))
    (m-wv/close id)))

(defun t--cleanup-sentinel (_conn)
  (setf (o-dying t--mgr) t)
  (setf (o-conn t--mgr) nil)
  (t--unregister-hooks)
  (dolist (buf (hash-table-values (o-buf-map t--mgr)))
    (when (buffer-live-p buf)
      (kill-buffer buf)))
  (clrhash (o-buf-map t--mgr))
  (clrhash (o-wv-map t--mgr))
  (clrhash (o-envs t--mgr)))

(defun t--notification-handler (_conn method params)
  (let* ((name (concat "emacs-webview2--recv-" (symbol-name method)))
         (sym (intern name)))
    (when (functionp sym)
      (funcall sym params))))

(defun t--start-webview2-manager ()
  "Start the Manager Subprocess and create the RPC connection."
  (when (not (t--alive-p))
    (let* ((path (file-name-concat t--dir "x64" "Debug" "wv2.exe"))
           (proc (make-process :name "WebView2-Manager"
                               :command `(,path)
                               :coding 'binary
                               :connection-type 'pipe)))
      (setf (o-conn t--mgr)
            (make-instance
             'jsonrpc-process-connection
             :name "Emacs-WebView2"
             :process proc
             :notification-dispatcher #'t--notification-handler
             :on-shutdown #'t--cleanup-sentinel))
    (setf (o-dying t--mgr) nil))))

(defun t--srpc (method params)
  (jsonrpc-request (o-conn t--mgr) method params))

(defun t--say (method params)
  (jsonrpc-notify (o-conn t--mgr) method params))

(cl-defun t--arpc (method params &key sf ef tf timeout)
  (jsonrpc-async-request
   (o-conn t--mgr) method params
   :success-fn sf :error-fn ef :timeout-fn tf
   :timeout timeout))

(defun m-echo (arg)
  (t--srpc 'echo arg))

(defun m-app/exit ()
  (setf (o-dying t--mgr) t)
  (t--say 'app/exit :jsonrpc-omit))

(defun m-env/create (config)
  (t--srpc 'env/create config))

(defun m-env/list-names ()
  (t--srpc 'env/list-names :jsonrpc-omit))

(defun m-wv/create (&optional hwnd visible rect url env-name)
  (let ((params `(,@(when hwnd `(:hwnd ,hwnd))
                  ,@(when visible `(:visible ,visible))
                  ,@(when rect `(:bounds ,rect))
                  ,@(when url `(:url ,url))
                  ,@(when env-name `(:environment ,env-name)))))
    (t--srpc 'wv/create params)))

(defun m-wv/close (id)
  (t--srpc 'wv/close `[,id]))

(defun m-wv/resize (id rect)
  (t--say 'wv/resize `[,id ,rect]))

(defun m-wv/reparent (id hwnd)
  (t--say 'wv/reparent `[,id ,hwnd]))

(defun m-wv/visible-p (id)
  (t--srpc 'wv/visible-p `[,id]))

(defun m-wv/set-visible (id b)
  (let ((bool (if b t :json-false)))
    (t--say 'wv/set-visible `[,id ,bool])))

(defun m-app/set-focus ()
  (t--say 'app/set-focus `[,(t--get-frame-hwnd)]))

(defun m-wv/get-title (id)
  (t--srpc 'wv/get-title `[,id]))

(defun m-wv/set-intercept-keys (id keys)
  (t--srpc 'wv/set-intercept-keys `[,id ,keys]))

(defun m-wv/focus (id)
  (t--say 'wv/focus `[,id]))

(defun m-wv/navigate (id url)
  (t--say 'wv/navigate `[,id ,url]))

(defun m-wv/sync-ui-batch (arg)
  (t--say 'wv/sync-ui-batch arg))

(defun n-input/event (params)
  (let* ((id (map-elt params :id))
         (key (map-elt params :key)))
    (o-focus-by-id id)
    (push (t--decode-uint-to-key key) unread-command-events)))

(defun n-wv/title-changed (params)
  (let* ((id (map-elt params :id))
         (title (map-elt params :title))
         (target-buf (o-get-buffer id)))
    (when (and target-buf (buffer-live-p target-buf))
      (with-current-buffer target-buf
        (let* ((new-name (format "W-%s" title)))
          (unless (string= (buffer-name) new-name)
            (rename-buffer new-name t)))))))

(defun n-wv/new-window-requested (params)
  (let* ((url (map-elt params :url)))
    (t-open-url url)))

(defun t-set-focus-on-click ()
  (when (and (t--alive-p)
             (eq this-command 'mouse-drag-region))
    (select-frame-set-input-focus (selected-frame))))

(defun o-sync-all-active-wv ()
  (when (t--alive-p)
    (let ((diffs nil))
      (maphash (lambda (_id wv)
                 (when-let* ((d (t--calculate-ui-diff wv)))
                   (push d diffs)))
               (o-wv-map t--mgr))
      (when diffs
        (prog1 t
          (m-wv/sync-ui-batch (vconcat diffs)))))))

(defun t-after-frame-delete (_frame)
  (o-sync-all-active-wv))

(defun t-on-delete-frame (frame)
  (when (t--alive-p)
    (let* ((batch nil))
      (maphash (lambda (_id wv)
                 (when (eq (t--webview-frame wv) frame)
                   (let ((cmd (vector (t--webview-id wv) 0 nil 0)))
                     (push cmd batch))
                   (setf (t--webview-frame wv) nil)
                   (setf (t--webview-last-visible wv) 0)))
               (o-wv-map t--mgr))
      (when batch
        (m-wv/sync-ui-batch (vconcat batch))))))

(defun t--try-focus-wv (window)
  (when (and (t--alive-p)
             (window-live-p window)
             (not (window-minibuffer-p window)))
    (with-current-buffer (window-buffer window)
      (when-let* ((id (bound-and-true-p t-wv))
                  (wv (gethash id (o-wv-map t--mgr)))
                  (_ (eq (t--webview-last-visible wv) 1)))
        (let* ((current-frame (window-frame window))
               (wv-frame (t--webview-frame wv)))
          (when (eq wv-frame current-frame)
            (m-wv/focus id)))))))

(defun t-on-window-state-change ()
  (when (o-sync-all-active-wv)
    (t--try-focus-wv (selected-window))))

(defalias 't-on-window-state-change-d
  (timeout-debounced-func #'t-on-window-state-change 0))

(defun t--register-hooks ()
  (add-hook 'pre-command-hook #'t-set-focus-on-click)
  (add-hook 'delete-frame-functions #'t-on-delete-frame)
  (add-hook 'after-delete-frame-functions #'t-after-frame-delete)
  (add-hook 'window-state-change-hook #'t-on-window-state-change-d))

(defun t--unregister-hooks ()
  (remove-hook 'pre-command-hook #'t-set-focus-on-click)
  (remove-hook 'delete-frame-functions #'t-on-delete-frame)
  (remove-hook 'after-delete-frame-functions #'t-after-frame-delete)
  (remove-hook 'window-state-change-hook #'t-on-window-state-change-d))

(defun t--tab-line-tabs ()
  (hash-table-values (o-buf-map t--mgr)))

(defun t--setup-tab-line ()
  (setq-local tab-line-tabs-function #'t--tab-line-tabs)
  (setq-local tab-line-close-tab-function #'kill-buffer)
  (setq-local tab-line-tab-name-function
              #'tab-line-tab-name-truncated-buffer)
  (tab-line-mode 1))

(defun t-open-url (url &optional env)
  (interactive "sUrl: ")
  (t--start-webview2-manager)
  (when (string-empty-p url)
    (setq url "https://google.com"))
  (let* ((buffer (generate-new-buffer "EWV2"))
         (_ (switch-to-buffer buffer))
         (rect (t--get-window-rect)))
    (o-spawn :buffer buffer :url url :rect rect)
    (with-current-buffer buffer
      (t--setup-tab-line))))

(defun t-navigate (url &optional buffer)
  (interactive "sNavigate to URL: ")
  (let ((buf (or buffer (current-buffer))))
    (with-current-buffer buf
      (if (and (t--alive-p) (bound-and-true-p t-wv))
          (m-wv/navigate t-wv url)
        (user-error "Current buffer is not a valid WebView2 buffer")))))

(defun t-shutdown ()
  (interactive)
  (when (t--alive-p) (m-app/exit)))

;; Local Variables:
;; read-symbol-shorthands: (("t-" . "emacs-webview2-")
;;                          ("m-" . "emacs-webview2--send-")
;;                          ("n-" . "emacs-webview2--recv-")
;;                          ("o-" . "emacs-webview2--manager-"))
;; coding: utf-8-unix
;; End:
