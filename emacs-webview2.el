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
(require 'seq)

(defgroup emacs-webview2 nil
  "Options for emacs webview2 binding."
  :tag "Emacs Webview2"
  :group 'applications)

(defcustom t-env-alist
  '(("default" . (:language "zh-CN"
                  :additional_browser_arguments nil
                  :user_data_dir nil)))
  "Docstring"
  :type '(alist :key-type string :value-type plist)
  :group 'emacs-webview2)

(defcustom t-default-env "default"
  "The deafult webview2 environment."
  :type 'string
  :group 'emacs-webview2)

(defcustom t-default-intercept-keys
  '("C-g" "M-x" "C-x" "M-:" "C-c" "C-[")
  "Webview2 intercept keys"
  :type '(repeat string)
  :group 'emacs-webview2)

(cl-defstruct t--webview
  "WebView2 instance wrapper."
  (id -1 :documentation "WebView2 object's id.")
  (frame nil :documentation "Frame object belongs to.")
  (url nil :documentation "Link navigate to.")
  (buffer nil :documentation "buffer the object belongs to."))

(defvar-local t--webview nil
  "Buffer-local Webview2 structure.")

(defconst t--dir
  (if (not load-in-progress) default-directory
    (file-name-directory load-file-name))
  "Package's root directory.")

(defvar t--conn nil
  "JSONRPC connection object to WebView2 manager process.")

(defvar t--shutting-down nil)

(defvar t--buffer-registry (make-hash-table :test 'eq)
  "Managed buffers.")

(defun t--register-buffer (id buffer)
  (puthash id buffer t--buffer-registry))

(defun t--get-buffer-by-id (id)
  (gethash id t--buffer-registry))

(defun t--get-buffers ()
  (hash-table-values t--buffer-registry))

(defun t--remove-buffer-by-id (id)
  (remhash id t--buffer-registry))

(defun t--clear-buffers ()
  (clrhash t--buffer-registry))

(defvar t--initialized-envs (make-hash-table :test 'equal)
  "Initialized env hashtable")

(defun t--add-env (env-name)
  (puthash env-name t t--initialized-envs))

(defun t--get-env-by-name (env-name)
  (gethash env-name t--initialized-envs))

(defun t--clear-env ()
  (clrhash t--initialized-envs))

(defun t--ensure-env (env-name)
  (unless (t--get-env-by-name env-name)
    (let ((config (cdr (assoc env-name t-env-alist))))
      (unless config
        (user-error "%s not exist, try to add it" env-name))
      (let ((rpc-params (append (list :name env-name) config)))
        (m-env/create rpc-params)))
    (t--add-env env-name)))

(defun t--alive-p ()
  "Check if the connection is alive"
  (and (jsonrpc-process-connection-p t--conn)
       (not t--shutting-down)
       (jsonrpc-running-p t--conn)))

(defun t--srpc (method params)
  "Send a synchronous JSON-RPC request METHOD with PARAMS.

This function wraps `jsonrpc-request' to communicate with the JSON-RPC
server via the connection object held in `emacs-webview--conn'.

Return the result of the remote method call, or signal an error if the
request fails or times out."
  (jsonrpc-request t--conn method params))

(defun t--say (method params)
  (jsonrpc-notify t--conn method params))

(defun t--get-frame-hwnd (&optional frame)
  "Get frame's Win32 HWND."
  (let ((id (frame-parameter (or frame (selected-frame)) 'window-id)))
    (string-to-number id)))

(defun t--get-window-rect (&optional window)
  "Get WINDOW bound rect."
  (cl-coerce (window-body-pixel-edges window) 'vector))

(defun m-app/exit ()
  (setq t--shutting-down t)
  (t--say 'app/exit :jsonrpc-omit))

(defun m-env/create (config)
  (t--srpc 'env/create config))

(defun m-env/list-names ()
  (t--srpc 'env/list-names :jsonrpc-omit))

(defun m-wv/create (rect &optional url env-name)
  (let ((hwnd (t--get-frame-hwnd)))
    (t--srpc 'wv/create `[,hwnd ,rect ,url ,env-name])))

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

(defun t-set-focus-on-click ()
  (when (eq this-command 'mouse-drag-region)
    (when (t--alive-p)
      ;; (m-app/set-focus))))
      ;; Just let Emacs directly get the focus.
      (select-frame-set-input-focus (selected-frame)))))

(defun t--set-focus-buffer-by-id (id)
  (when-let* ((target-buf (t--get-buffer-by-id id)))
    (let ((target-win (get-buffer-window target-buf 'visible)))
      (if target-win (select-window target-win)
        (switch-to-buffer target-buf))
      (let ((frame (window-frame (selected-window))))
        (select-frame-set-input-focus frame)))))

(defun n-input/event (params)
  (let* ((id (map-elt params :id))
         (key (map-elt params :key)))
    (t--set-focus-buffer-by-id id)
    (push (t--decode-uint-to-key key) unread-command-events)))

(defun n-wv/title-changed (params)
  (let* ((id (map-elt params :id))
         (title (map-elt params :title)))
    (when-let* ((target-buf (t--get-buffer-by-id id)))
      (with-current-buffer target-buf
        (let* ((new-name (format "WV2-%s" title)))
          (unless (string= (buffer-name) new-name)
            (rename-buffer new-name t)))))))

(defun n-wv/new-window-requested (params)
  (let* ((url (map-elt params :url)))
    (t-open-url url)))

(defvar t--vkey-map
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

(defvar t--modifier-value-map
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
  (seq-reduce
   (lambda (s a)
     (+ s (or (alist-get a t--modifier-value-map) 0)))
   ms 0))

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

(defun t--set-intercept-keys (id keys)
  (let* ((ls (mapcar #'t--encode-key-to-uint keys)))
    (m-wv/set-intercept-keys id (vconcat ls))))

(defun t-monitor-window-configuration-change ()
  (when (t--alive-p)
    (let ((processed-ids))
      (dolist (wnd (window-list))
        (with-selected-window wnd
          (when-let* ((w t--webview)
                      (id (t--webview-id w)))
            (unless (memq id processed-ids)
              (push id processed-ids)
              (let* ((bounds (t--get-window-rect))
                     (frame (window-frame wnd))
                     (hwnd (t--get-frame-hwnd frame)))
                (unless (eq (t--webview-frame w) frame)
                  (m-wv/reparent id hwnd)
                  (setf (t--webview-frame w) frame))
                (m-wv/resize id bounds)
                (m-wv/set-visible id t))))))
      (maphash (lambda (id buf)
                 (unless (memq id processed-ids)
                   (when (buffer-live-p buf)
                     (m-wv/set-visible id nil))))
               t--buffer-registry))))

(defun t-after-frame-delete (_frame)
  (t--monitor-window-configuration-change))

(defun t-rescue-webview-on-frame-delete (frame)
  (when (t--alive-p)
    (dolist (wnd (window-list frame))
      (with-current-buffer (window-buffer wnd)
        (when-let* ((w t--webview)
                    (_ (eq (t--webview-frame w) frame))
                    (safe-frame (car (remove frame (frame-list))))
                    (hwnd (t--get-frame-hwnd safe-frame))
                    (id (t--webview-id w)))
          (m-wv/reparent id hwnd)
          (m-wv/set-visible id nil)
          (setf (t--webview-frame w) safe-frame))))))

(defun t-give-focus-on-window-selection-change (&optional _window)
  (when (and (t--alive-p)
             (bound-and-true-p t--webview)
             (t--webview-p t--webview))
    (let* ((id (t--webview-id t--webview)))
      (m-wv/focus id))))

(defun t-give-focus-on-window-buffer-change (_window)
  (when (and (t--alive-p)
             (bound-and-true-p t--webview)
             (t--webview-p t--webview))
    (let ((id (t--webview-id t--webview)))
      (m-wv/focus id))))

(defun t--register-hooks ()
  (add-hook 'pre-command-hook #'t-set-focus-on-click)
  (add-hook 'delete-frame-functions
            #'t-rescue-webview-on-frame-delete)
  (add-hook 'window-configuration-change-hook
            #'t-monitor-window-configuration-change)
  (add-hook 'after-delete-frame-functions
            #'t-after-frame-delete)
  (add-hook 'window-selection-change-functions
            #'t-give-focus-on-window-selection-change)
  (add-hook 'window-buffer-change-functions
            #'t-give-focus-on-window-buffer-change))

(defun t--unregister-hooks ()
  (remove-hook 'pre-command-hook #'t-set-focus-on-click)
  (remove-hook 'delete-frame-functions
               #'t-rescue-webview-on-frame-delete)
  (remove-hook 'window-configuration-change-hook
               #'t-monitor-window-configuration-change)
  (remove-hook 'after-delete-frame-functions
               #'t-after-frame-delete)
  (remove-hook 'window-selection-change-functions
               #'t-give-focus-on-window-selection-change)
  (remove-hook 'window-buffer-change-functions
               #'t-give-focus-on-window-buffer-change))

(defun t--cleanup-sentinel (_conn)
  (setq t--shutting-down t)
  (dolist (buf (t--get-buffers))
    (when (buffer-live-p buf)
      (kill-buffer buf)))
  (t--unregister-hooks)
  (t--clear-buffers)
  (t--clear-env)
  (setq t--conn nil))

(defun t--notification-handler (_conn method params)
  (let* ((name (concat "emacs-webview2--recv-" (symbol-name method)))
         (sym (intern name)))
    (when (functionp sym)
      (funcall sym params))))

(defun t--start-webview2-manager ()
  "Start the Manager Subprocess and create the RPC connection."
  (when-let* ((_ (not (t--alive-p)))
              (path (file-name-concat t--dir "x64" "Debug" "wv2.exe"))
              (proc (make-process :name "WebView2-Manager"
                                  :command `(,path)
                                  :coding 'binary)))
    (setq t--conn (make-instance 'jsonrpc-process-connection
                                 :name "Emacs-WebView2"
                                 :process proc
                                 :notification-dispatcher #'t--notification-handler
                                 :on-shutdown #'t--cleanup-sentinel))
    (setq t--shutting-down nil)
    (t--register-hooks)))

(defun t--on-kill-buffer ()
  (when (and (t--alive-p) (t--webview-p t--webview))
    (let ((id (t--webview-id t--webview)))
      (t--remove-buffer-by-id id)
      (m-wv/close id))))

(defun t--setup-buffer (obj)
  (setq t--webview obj)
  (t--register-buffer (t--webview-id obj) (current-buffer))
  (add-hook 'kill-buffer-hook 't--on-kill-buffer nil t))

(defun t--tab-line-tabs ()
  (t--get-buffers))

(defun t-open-url (url &optional env-name)
  (interactive "sUrl: ")
  (t--start-webview2-manager)
  (let* ((use-env (or env-name t-default-env)))
    (t--ensure-env use-env)
    (when (string-empty-p url)
      (setq url "https://google.com"))
    (let ((buffer (generate-new-buffer "EWV2")))
      (switch-to-buffer buffer)
      (let* ((rect (t--get-window-rect))
             (id (m-wv/create rect url use-env))
             (wobj (make-emacs-webview2--webview
                    :id id :frame (selected-frame)
                    :url url :buffer buffer)))
        (t--set-intercept-keys id t-default-intercept-keys)
        (with-current-buffer buffer
          (setq-local tab-line-tabs-function #'t--tab-line-tabs)
          (setq-local tab-line-close-tab-function #'kill-buffer)
          (setq-local tab-line-tab-name-function #'tab-line-tab-name-truncated-buffer)
          (tab-line-mode)
          (t--setup-buffer wobj))))))

(defun t-navigate (url &optional buffer)
  (interactive "sNavigate to URL: ")
  (let ((buf (or buffer (current-buffer))))
    (with-current-buffer buf
      (if (and t--webview (t--alive-p))
          (m-wv/navigate (t--webview-id t--webview) url)
        (user-error "Current buffer is not a valid WebView2 buffer")))))

(defun t-shutdown ()
  (interactive)
  (when (t--alive-p) (m-app/exit)))

;; Local Variables:
;; read-symbol-shorthands: (("t-" . "emacs-webview2-")
;;                          ("m-" . "emacs-webview2--send-")
;;                          ("n-" . "emacs-webview2--recv-"))
;; coding: utf-8-unix
;; End:
