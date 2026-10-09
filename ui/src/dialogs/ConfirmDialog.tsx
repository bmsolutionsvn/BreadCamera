import { Modal } from '../components/Modal';

// Destructive actions never run at once: title states the consequence, the button names the verb (UX contract §8.4).
export function ConfirmDialog({ title, body, confirm, onConfirm, onCancel, danger = true }: { title: string; body: string; confirm: string; onConfirm: () => void; onCancel: () => void; danger?: boolean }) {
  return (
    <Modal
      title={title}
      onClose={onCancel}
      footer={
        <>
          <button className="btn" onClick={onCancel}>
            Cancel
          </button>
          <button className={`btn ${danger ? 'danger' : 'primary'}`} onClick={onConfirm}>
            {confirm}
          </button>
        </>
      }
    >
      <p>{body}</p>
    </Modal>
  );
}
