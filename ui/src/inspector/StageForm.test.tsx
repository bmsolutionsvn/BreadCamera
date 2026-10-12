import { fireEvent, render, screen } from '@testing-library/react';

import type { StageSchemaJson } from '../api/protocol';
import { StageForm } from './StageForm';

const schema: StageSchemaJson = {
  kind: 'matte',
  label: 'Background removal',
  description: 'AI person cut-out without a green screen.',
  icon: 'matte',
  requires: [],
  onAdd: { enabled: true },
  fields: [
    { key: 'enabled', type: 'bool', default: false, label: 'On' },
    { key: 'quality', type: 'enum', default: 'quality', label: 'Quality', options: ['quality', 'performance'], optionLabels: ['High quality', 'Performance'] },
    { key: 'choke', type: 'number', default: 0, label: 'Choke (px)', min: -8, max: 8, step: 1 },
  ],
};

test('renders every setting except the on/off switch', () => {
  render(<StageForm schema={schema} values={{ enabled: true, quality: 'quality', choke: 0 }} onChange={() => {}} />);
  expect(screen.queryByLabelText('On')).toBeNull();
  expect(screen.getByLabelText('Quality')).toHaveValue('quality');
  expect(screen.getByLabelText('Choke (px)')).toHaveValue('0');
});

test('a change reports only that key', () => {
  const onChange = vi.fn();
  render(<StageForm schema={schema} values={{ enabled: true, quality: 'quality', choke: 0 }} onChange={onChange} />);
  fireEvent.change(screen.getByLabelText('Quality'), { target: { value: 'performance' } });
  expect(onChange).toHaveBeenCalledWith({ quality: 'performance' });
});
